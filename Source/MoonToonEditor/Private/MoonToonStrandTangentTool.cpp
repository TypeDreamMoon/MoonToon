// Copyright Dream Moon. All Rights Reserved.

#include "MoonToonStrandTangentTool.h"

#include "Components/MeshComponent.h"
#include "DrawDebugHelpers.h"
#include "Editor.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Materials/MaterialInterface.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "MoonToonEditorBPLibrary.h"
#include "MoonToonMeshTargets.h"
#include "MoonToonSmoothNormalTool.h"
#include "MoonToonStrandPreviewActor.h"
#include "Serialization/BufferArchive.h"
#include "Serialization/MemoryReader.h"

#define LOCTEXT_NAMESPACE "MoonToonStrandTangentTool"

namespace
{
	/** Disjoint-set over vertex indices. Sibling of the outline width tool's island pass: both split
	 *  hair into cards the same way, but this one keeps the axis itself where that one keeps the
	 *  projection along it, so the middle stages have nothing to share. */
	class FStrandUnionFind
	{
	public:
		explicit FStrandUnionFind(int32 Num)
		{
			Parent.SetNum(Num);
			for (int32 Index = 0; Index < Num; ++Index)
			{
				Parent[Index] = Index;
			}
		}

		int32 Find(int32 Index)
		{
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

	/** Dominant eigenvector of a symmetric 3x3 covariance matrix, by power iteration. Hair cards are
	 *  strongly elongated, so the dominant axis is well separated and converges in a few steps. */
	FVector3f StrandDominantAxis(const FVector3f Covariance[3])
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
				return FVector3f::ZeroVector; // Degenerate: no meaningful axis, caller skips.
			}
			Axis = Next.GetSafeNormal();
		}
		return Axis;
	}

	struct FIslandAxisResult
	{
		/** Per-VERTEX island axis in mesh local space. Zero for skipped islands. */
		TArray<FVector3f> PerVertexAxis;

		/** Per-VERTEX island key (the union-find root), INDEX_NONE for skipped islands. The preview
		 *  colours by this, which doubles as a check that the island split matches the hair cards. */
		TArray<int32> PerVertexIsland;

		int32 NumIslands = 0;
		int32 NumSkippedIslands = 0;
	};

	/** Splits the masked geometry into position-welded, face-connected islands and hands every
	 *  vertex its island's dominant axis. */
	FIslandAxisResult ComputeIslandAxes(
		const TArray<FVector3f>& Positions,
		const TArray<int32>& VertexIndices,
		const FMoonToonLODFaces& Faces,
		const FMoonToonWedgeMask& Mask,
		int32 MinIslandVertices)
	{
		FIslandAxisResult Result;
		Result.PerVertexAxis.Init(FVector3f::ZeroVector, Positions.Num());
		Result.PerVertexIsland.Init(INDEX_NONE, Positions.Num());

		// Weld by exact position first: import data stores one point per UV/normal seam, and without
		// the weld every card shatters into strips along its seams.
		FStrandUnionFind Islands(Positions.Num());
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

		// Connect through faces the section filter includes -- a card touching the scalp must not
		// merge into the head and average its axis away.
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
				continue;
			}

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

			const FVector3f Axis = StrandDominantAxis(Covariance);
			if (Axis.IsNearlyZero())
			{
				++Result.NumSkippedIslands;
				continue;
			}
			++Result.NumIslands;

			for (int32 VertexIndex : Members)
			{
				Result.PerVertexAxis[VertexIndex] = Axis;
				Result.PerVertexIsland[VertexIndex] = Island.Key;
			}
		}

		return Result;
	}

	/** Projects the island axis onto the wedge's tangent plane. Zero when no meaningful in-plane
	 *  direction exists (a card end-cap face whose normal is parallel to the axis). */
	FVector3f ProjectAxisToTangentPlane(const FVector3f& Axis, const FVector3f& Normal)
	{
		const FVector3f Projected = Axis - Normal * (Axis | Normal);
		return Projected.IsNearlyZero() ? FVector3f::ZeroVector : Projected.GetSafeNormal();
	}

	// --- Ellipsoid field (MooaToon-style ring) ----------------------------------------------------

	struct FStrandEllipsoid
	{
		FVector Center = FVector::ZeroVector;
		FVector Radii = FVector(12.0, 12.0, 14.0);
		FQuat Rotation = FQuat::Identity;
	};

	/** The tool's ellipsoid in mesh local space: fitted to the masked geometry's bounds (then offset,
	 *  scaled and rotated by the tool settings) or taken verbatim from the manual fields. */
	FStrandEllipsoid ResolveStrandEllipsoid(
		const UMoonToonStrandTangentTool& Tool,
		const TArray<FVector3f>& Positions,
		const TArray<int32>& VertexIndices,
		const FMoonToonWedgeMask& Mask)
	{
		FStrandEllipsoid Ellipsoid;
		Ellipsoid.Rotation = Tool.EllipsoidRotation.Quaternion();

		if (Tool.bFitEllipsoidToSelection)
		{
			FBox Bounds(ForceInit);
			for (int32 WedgeIndex = 0; WedgeIndex < VertexIndices.Num(); ++WedgeIndex)
			{
				if (Mask.Contains(WedgeIndex) && Positions.IsValidIndex(VertexIndices[WedgeIndex]))
				{
					Bounds += FVector(Positions[VertexIndices[WedgeIndex]]);
				}
			}
			if (Bounds.IsValid)
			{
				Ellipsoid.Center = Bounds.GetCenter();
				Ellipsoid.Radii = Bounds.GetExtent() * Tool.EllipsoidRadiiScale;
			}
		}
		else
		{
			Ellipsoid.Center = Tool.EllipsoidCenter;
			Ellipsoid.Radii = Tool.EllipsoidRadii;
		}

		Ellipsoid.Center += Tool.EllipsoidCenterOffset;
		Ellipsoid.Radii = Ellipsoid.Radii.ComponentMax(FVector(0.1));
		return Ellipsoid;
	}

	/**
	 * The meridian (pole-to-pole) tangent of the ellipsoid, evaluated at a vertex's angular position
	 * and transferred as-is -- deliberately NOT projected onto the hair surface. The whole point of
	 * the transfer is that the field stays as smooth as the ellipsoid regardless of how messy the
	 * cards are; projecting would let the card topology back in.
	 */
	FIslandAxisResult ComputeEllipsoidField(
		const TArray<FVector3f>& Positions,
		const TArray<int32>& VertexIndices,
		const FMoonToonWedgeMask& Mask,
		const FStrandEllipsoid& Ellipsoid)
	{
		FIslandAxisResult Result;
		Result.PerVertexAxis.Init(FVector3f::ZeroVector, Positions.Num());
		Result.PerVertexIsland.Init(INDEX_NONE, Positions.Num());
		Result.NumIslands = 1;

		const FQuat InverseRotation = Ellipsoid.Rotation.Inverse();

		for (int32 WedgeIndex = 0; WedgeIndex < VertexIndices.Num(); ++WedgeIndex)
		{
			const int32 VertexIndex = VertexIndices[WedgeIndex];
			if (!Mask.Contains(WedgeIndex) || !Positions.IsValidIndex(VertexIndex)
				|| Result.PerVertexIsland[VertexIndex] != INDEX_NONE)
			{
				continue;
			}

			const FVector Local = InverseRotation.RotateVector(FVector(Positions[VertexIndex]) - Ellipsoid.Center);
			const FVector UnitDir = (Local / Ellipsoid.Radii).GetSafeNormal();
			if (UnitDir.IsNearlyZero())
			{
				continue; // Vertex sits at the centre; no angular position, no direction.
			}

			// Direction toward the +Z pole within the unit sphere's tangent plane. At the poles the
			// meridian is undefined; fall back to the projected X axis so pole caps stay smooth-ish.
			FVector Meridian = FVector::ZAxisVector - UnitDir * (UnitDir | FVector::ZAxisVector);
			if (Meridian.IsNearlyZero())
			{
				Meridian = FVector::XAxisVector - UnitDir * (UnitDir | FVector::XAxisVector);
			}
			Meridian.Normalize();

			// Scale by the radii to follow the ellipsoid's actual surface curve, rotate back out.
			const FVector SurfaceTangent = Ellipsoid.Rotation.RotateVector((Meridian * Ellipsoid.Radii).GetSafeNormal());
			Result.PerVertexAxis[VertexIndex] = FVector3f(SurfaceTangent);
			Result.PerVertexIsland[VertexIndex] = 0;
		}

		return Result;
	}

	/** Signed direction as a colour, so the preview shows the field's smoothness at a glance. */
	FColor StrandDirectionColor(const FVector3f& Direction)
	{
		return FColor(
			static_cast<uint8>((Direction.X * 0.5f + 0.5f) * 255.0f),
			static_cast<uint8>((Direction.Y * 0.5f + 0.5f) * 255.0f),
			static_cast<uint8>((Direction.Z * 0.5f + 0.5f) * 255.0f));
	}

	void DrawEllipsoidWireframe(
		UWorld* World, const FTransform& MeshToWorld, const FStrandEllipsoid& Ellipsoid,
		bool bPersistent, float LifeTime)
	{
		const FColor WireColor(200, 200, 200);
		constexpr int32 NumSegments = 48;

		auto DrawRing = [&](TFunctionRef<FVector(double)> PointAt)
		{
			FVector Previous = PointAt(0.0);
			for (int32 Segment = 1; Segment <= NumSegments; ++Segment)
			{
				const double Angle = 2.0 * PI * Segment / NumSegments;
				const FVector Current = PointAt(Angle);
				DrawDebugLine(World,
					MeshToWorld.TransformPosition(Previous), MeshToWorld.TransformPosition(Current),
					WireColor, bPersistent, LifeTime, /*DepthPriority=*/0, /*Thickness=*/0.0f);
				Previous = Current;
			}
		};

		auto OnEllipsoid = [&](const FVector& UnitPoint)
		{
			return Ellipsoid.Center + Ellipsoid.Rotation.RotateVector(UnitPoint * Ellipsoid.Radii);
		};

		// Two meridian rings and the equator, plus two latitude rings: enough to read position,
		// radii and tilt without flooding the viewport.
		DrawRing([&](double A) { return OnEllipsoid(FVector(FMath::Cos(A), 0.0, FMath::Sin(A))); });
		DrawRing([&](double A) { return OnEllipsoid(FVector(0.0, FMath::Cos(A), FMath::Sin(A))); });
		DrawRing([&](double A) { return OnEllipsoid(FVector(FMath::Cos(A), FMath::Sin(A), 0.0)); });
		for (const double LatitudeZ : { 0.5, -0.5 })
		{
			const double LatitudeRadius = FMath::Sqrt(1.0 - LatitudeZ * LatitudeZ);
			DrawRing([&](double A)
			{
				return OnEllipsoid(FVector(
					FMath::Cos(A) * LatitudeRadius, FMath::Sin(A) * LatitudeRadius, LatitudeZ));
			});
		}
	}

	TArray<int32> ResolveStrandLODRange(UObject* Mesh, bool bAllLODs, int32 LODIndex)
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

	// --- Channel backup ---------------------------------------------------------------------------
	// The bake overwrites channels whose original content cannot be recomputed (VRM import residue,
	// or a face-forward bake), so the first bake snapshots them and Restore plays the snapshot back.
	// One file per (mesh, LOD) under Saved/, deliberately outside Content: it is working state, not
	// an asset.

	constexpr uint32 GStrandBackupMagic = 0x4D545556; // 'MTUV'
	constexpr int32 GStrandBackupVersion = 1;

	FString StrandBackupFilePath(const UObject* Mesh, int32 LOD)
	{
		FString Safe = Mesh->GetPathName();
		for (TCHAR& Char : Safe)
		{
			if (!FChar::IsAlnum(Char) && Char != TEXT('_') && Char != TEXT('-'))
			{
				Char = TEXT('_');
			}
		}
		return FPaths::ProjectSavedDir() / TEXT("MoonToon/ChannelBackups")
			/ FString::Printf(TEXT("%s_LOD%d.uvbak"), *Safe, LOD);
	}

	bool SaveStrandBackup(const FString& FilePath, const FString& MeshPath, int32 LOD,
		const TArray<FVector2f>& UV1s, const TArray<FVector2f>& UV2s)
	{
		FBufferArchive Ar;
		uint32 Magic = GStrandBackupMagic;
		int32 Version = GStrandBackupVersion;
		FString Path = MeshPath;
		int32 LODCopy = LOD;
		int32 NumWedges = UV1s.Num();
		Ar << Magic << Version << Path << LODCopy << NumWedges;
		for (int32 Index = 0; Index < NumWedges; ++Index)
		{
			float X = UV1s[Index].X;
			float Y = UV1s[Index].Y;
			float Z = UV2s.IsValidIndex(Index) ? UV2s[Index].X : 0.0f;
			Ar << X << Y << Z;
		}
		return FFileHelper::SaveArrayToFile(Ar, *FilePath);
	}

	bool LoadStrandBackup(const FString& FilePath, int32& OutNumWedges, TArray<FVector3f>& OutTriplets)
	{
		TArray<uint8> Bytes;
		if (!FFileHelper::LoadFileToArray(Bytes, *FilePath))
		{
			return false;
		}

		FMemoryReader Ar(Bytes);
		uint32 Magic = 0;
		int32 Version = 0;
		FString MeshPath;
		int32 LOD = 0;
		Ar << Magic << Version << MeshPath << LOD << OutNumWedges;
		if (Magic != GStrandBackupMagic || Version != GStrandBackupVersion || OutNumWedges < 0)
		{
			return false;
		}

		OutTriplets.SetNumUninitialized(OutNumWedges);
		for (int32 Index = 0; Index < OutNumWedges; ++Index)
		{
			float X = 0.0f, Y = 0.0f, Z = 0.0f;
			Ar << X << Y << Z;
			OutTriplets[Index] = FVector3f(X, Y, Z);
		}
		return !Ar.IsError();
	}

	/**
	 * The write half of a bake for one LOD: first-run channel backup, the wedge loop, the mesh
	 * write, and the report line. Shared by the panel's Bake mode and the live-preview actor's
	 * Bake From Ellipsoid button so the two can never drift.
	 */
	void BakeChannelsForLOD(
		UObject* Mesh, int32 LOD,
		const FMoonToonWedgeMask& Mask,
		const FIslandAxisResult& Axes,
		bool bFieldAsIs,
		const FString& SourceNote,
		TArray<FVector3f>& Positions, TArray<int32>& VertexIndices,
		TArray<FVector3f>& Normals, TArray<FVector3f>& Tangents, TArray<FVector3f>& Binormals,
		TArray<FColor>& Colors,
		TArray<FVector2f>& UV0s, TArray<FVector2f>& UV1s, TArray<FVector2f>& UV2s, TArray<FVector2f>& UV3s,
		TArray<FString>& OutLines)
	{
		const int32 NumWedges = VertexIndices.Num();

		// Snapshot the channels before the first modification. Taken after any build-settings fix
		// on purpose: a reimport rewrites the channels from source anyway, so the state worth
		// returning to is the one this bake is about to overwrite.
		const FString BackupPath = StrandBackupFilePath(Mesh, LOD);
		if (!FPaths::FileExists(BackupPath))
		{
			if (SaveStrandBackup(BackupPath, Mesh->GetPathName(), LOD, UV1s, UV2s))
			{
				OutLines.Add(FString::Printf(TEXT("  %s LOD%d: original channels backed up to %s"),
					*Mesh->GetName(), LOD, *BackupPath));
			}
			else
			{
				OutLines.Add(FString::Printf(
					TEXT("  %s LOD%d: FAILED to write the channel backup (%s) -- baking anyway; "
						"Restore will not be available."),
					*Mesh->GetName(), LOD, *BackupPath));
			}
		}
		else
		{
			OutLines.Add(FString::Printf(
				TEXT("  %s LOD%d: keeping the existing backup (pre-first-bake original)."),
				*Mesh->GetName(), LOD));
		}

		int32 NumWritten = 0;
		int32 NumDegenerate = 0;
		for (int32 WedgeIndex = 0; WedgeIndex < NumWedges; ++WedgeIndex)
		{
			if (!Mask.Contains(WedgeIndex))
			{
				continue;
			}

			const int32 VertexIndex = VertexIndices[WedgeIndex];
			if (!Axes.PerVertexAxis.IsValidIndex(VertexIndex))
			{
				continue;
			}

			const FVector3f Axis = Axes.PerVertexAxis[VertexIndex];
			if (Axis.IsNearlyZero())
			{
				// Skipped island: existing channel data stays as it was rather than gaining a
				// random direction.
				continue;
			}

			const FVector3f Normal = Normals.IsValidIndex(WedgeIndex)
				? Normals[WedgeIndex] : FVector3f(0.0f, 0.0f, 1.0f);
			// The ellipsoid field is transferred as-is; only the per-card axis needs projecting
			// onto the surface.
			const FVector3f StrandLocal = bFieldAsIs ? Axis : ProjectAxisToTangentPlane(Axis, Normal);
			if (StrandLocal.IsNearlyZero())
			{
				// Axis is perpendicular to the surface here (a card end-cap face). No meaningful
				// in-plane strand direction exists; keep whatever the channels held.
				++NumDegenerate;
				continue;
			}

			const FMatrix TangentToLocal = UMoonToonSmoothNormalTool::BuildTangentToLocal(
				WedgeIndex, Mesh, Normals, Tangents, Binormals);
			const FVector TangentSpaceDir =
				TangentToLocal.InverseTransformVector(FVector(StrandLocal)).GetSafeNormal();
			const FVector Encoded = TangentSpaceDir * 0.5 + FVector(0.5);

			// Face-forward's channel set: UV1.xy + UV2.x, with UV2.y preserved for the
			// smoothed-normal bake that owns it.
			if (UV1s.IsValidIndex(WedgeIndex))
			{
				UV1s[WedgeIndex] = FVector2f(static_cast<float>(Encoded.X), static_cast<float>(Encoded.Y));
			}
			if (UV2s.IsValidIndex(WedgeIndex))
			{
				UV2s[WedgeIndex] = FVector2f(static_cast<float>(Encoded.Z), UV2s[WedgeIndex].Y);
			}
			++NumWritten;
		}

		if (NumWritten > 0)
		{
			// UV1 and UV2 only -- this bake owns UV1.xy + UV2.x, same channels as face-forward.
			MoonToonMesh::WriteMeshChannels(Mesh, LOD, nullptr, &UV1s, &UV2s);
		}

		OutLines.Add(FString::Printf(
			TEXT("  %s LOD%d: %s, %d/%d wedges written%s."),
			*Mesh->GetName(), LOD, *SourceNote, NumWritten, NumWedges,
			NumDegenerate > 0
				? *FString::Printf(TEXT(", %d degenerate kept"), NumDegenerate) : TEXT("")));
	}

	// --- Preview ----------------------------------------------------------------------------------

	/** Stable island colour: spread hues with a golden-angle-ish multiplier so neighbouring island
	 *  keys land far apart on the wheel. */
	FColor StrandIslandColor(int32 IslandKey)
	{
		const uint8 Hue = static_cast<uint8>((static_cast<uint32>(IslandKey) * 137u) & 0xFFu);
		return FLinearColor::MakeFromHSV8(Hue, 220, 255).ToFColor(/*bSRGB=*/true);
	}
}

FText UMoonToonStrandTangentTool::GetToolName() const
{
	return LOCTEXT("StrandTangentName", "Bake Strand Tangent");
}

FText UMoonToonStrandTangentTool::GetToolDescription() const
{
	return LOCTEXT("StrandTangentDesc",
		"Bakes a per-vertex strand direction into UV1.xy / UV2.x, in tangent space, for the Kajiya "
		"hair specular.\n\n"
		"ELLIPSOID (default) is MooaToon's stylized-highlight method with the Houdini step moved "
		"in-engine: directions come from a virtual ellipsoid's pole-to-pole field, so the highlight "
		"is one smooth ring you shape by offsetting, scaling and tilting the ellipsoid. ISLAND PCA "
		"instead uses each hair card's own dominant axis -- true strand flow, breaks per card.\n\n"
		"PREVIEW (default) draws the directions in the viewport (ellipsoid mode also draws the "
		"ellipsoid wireframe) and writes nothing. BAKE snapshots the original channels on first run "
		"(Saved/MoonToon/ChannelBackups/); RESTORE plays that snapshot back.\n\n"
		"Run on the HAIR sections only: the face-forward bake stores its own data in the same "
		"channels on the face.");
}

FText UMoonToonStrandTangentTool::GetRunLabel() const
{
	switch (Mode)
	{
	case EMoonToonStrandTangentMode::Preview:
		return LOCTEXT("StrandTangentPreview", "Preview Directions");
	case EMoonToonStrandTangentMode::LivePreview:
		return IsLivePreviewActive()
			? LOCTEXT("StrandTangentStopLivePreview", "Stop Live Preview")
			: LOCTEXT("StrandTangentLivePreview", "Start Live Preview");
	case EMoonToonStrandTangentMode::Restore:
		return LOCTEXT("StrandTangentRestore", "Restore Pre-Bake Channels");
	default:
		return LOCTEXT("StrandTangentRun", "Bake Strand Tangent");
	}
}

FString UMoonToonStrandTangentTool::Run(const FMoonToonToolContext& Context)
{
	TArray<UObject*> Meshes;
	FString Report;
	if (!ResolveMeshes(Context, Meshes, Report))
	{
		return Report;
	}

	TArray<FString> Lines;

	// --- Restore: play the pre-bake snapshot back and stop. -------------------------------------
	if (Mode == EMoonToonStrandTangentMode::Restore)
	{
		Lines.Add(TEXT("Restore Pre-Bake Channels (whole-channel: the section filter does not apply)"));
		for (UObject* Mesh : Meshes)
		{
			FMoonToonScopedMeshEdit BatchedEdit(Mesh);
			for (int32 LOD : ResolveStrandLODRange(Mesh, bAllLODs, LODIndex))
			{
				const FString BackupPath = StrandBackupFilePath(Mesh, LOD);
				int32 BackupWedges = 0;
				TArray<FVector3f> Triplets;
				if (!LoadStrandBackup(BackupPath, BackupWedges, Triplets))
				{
					Lines.Add(FString::Printf(TEXT("  %s LOD%d: no backup found -- nothing to restore."),
						*Mesh->GetName(), LOD));
					continue;
				}

				TArray<FVector3f> Positions, Normals, Tangents, Binormals;
				TArray<int32> VertexIndices;
				TArray<FColor> Colors;
				TArray<FVector2f> UV0s, UV1s, UV2s, UV3s;
				UMoonToonEditorBPLibrary::MoonGetMeshData(Mesh, LOD, Positions, VertexIndices, Normals,
					Tangents, Binormals, Colors, UV0s, UV1s, UV2s, UV3s);

				if (VertexIndices.Num() != BackupWedges)
				{
					Lines.Add(FString::Printf(
						TEXT("  %s LOD%d: backup has %d wedges but the mesh now has %d (reimported since?) "
							"-- refusing to restore."),
						*Mesh->GetName(), LOD, BackupWedges, VertexIndices.Num()));
					continue;
				}

				for (int32 WedgeIndex = 0; WedgeIndex < BackupWedges; ++WedgeIndex)
				{
					const FVector3f& Triplet = Triplets[WedgeIndex];
					if (UV1s.IsValidIndex(WedgeIndex))
					{
						UV1s[WedgeIndex] = FVector2f(Triplet.X, Triplet.Y);
					}
					if (UV2s.IsValidIndex(WedgeIndex))
					{
						UV2s[WedgeIndex] = FVector2f(Triplet.Z, UV2s[WedgeIndex].Y);
					}
				}

				// UV1 and UV2 only -- see above.
				MoonToonMesh::WriteMeshChannels(Mesh, LOD, nullptr, &UV1s, &UV2s);
				Lines.Add(FString::Printf(TEXT("  %s LOD%d: restored %d wedges from %s"),
					*Mesh->GetName(), LOD, BackupWedges, *BackupPath));
			}
		}
		Lines.Add(TEXT(""));
		Lines.Add(TEXT("The backup file is kept, so Restore can be repeated after another bake."));
		return FString::Join(Lines, TEXT("\n"));
	}

	// --- Live preview: hand control to the gizmo actor and return. --------------------------------
	if (Mode == EMoonToonStrandTangentMode::LivePreview)
	{
		// Second press = stop. The actor also has its own Exit button, but the panel is where the
		// preview was started, so it has to be able to end it too -- especially since the actor
		// deliberately does not retarget the panel any more.
		if (IsLivePreviewActive())
		{
			const int32 NumStopped = StopLivePreview();
			return FString::Printf(
				TEXT("Live preview stopped (%d actor(s) removed); the hair materials are back to normal."),
				NumStopped);
		}

		if (TangentSource != EMoonToonStrandTangentSource::Ellipsoid)
		{
			return TEXT("Live Preview drives the ellipsoid workflow -- switch Tangent Source to "
				"Ellipsoid first (the per-card PCA source has the line preview instead).");
		}

		UObject* Mesh = Meshes[0];
		UMeshComponent* Component = MoonToonMesh::FindPlacedMeshComponent(Mesh);
		if (!Component)
		{
			return FString::Printf(TEXT("%s is not placed in the level. The live preview overrides a "
				"placed component's materials, so drop the mesh into the level first."),
				*Mesh->GetName());
		}

		UMaterialInterface* BandMaterial = LoadObject<UMaterialInterface>(nullptr,
			TEXT("/MoonToon/Materials/Debug/M_MoonToonStrandPreview.M_MoonToonStrandPreview"));
		UMaterialInterface* ShellMaterial = LoadObject<UMaterialInterface>(nullptr,
			TEXT("/MoonToon/Materials/Debug/M_MoonToonStrandPreviewShell.M_MoonToonStrandPreviewShell"));
		if (!BandMaterial)
		{
			return TEXT("/MoonToon/Materials/Debug/M_MoonToonStrandPreview is missing -- compile "
				"M_MoonToonStrandPreview.dsm first.");
		}

		UWorld* World = Component->GetWorld();

		// One preview at a time; destroying the old actor also restores its material overrides.
		for (TActorIterator<AMoonToonStrandPreviewActor> It(World); It; ++It)
		{
			It->Destroy();
		}

		// Fit the starting ellipsoid to the selected sections so the actor spawns wrapped around
		// the hair instead of at the origin.
		TArray<FVector3f> Positions, Normals, Tangents, Binormals;
		TArray<int32> VertexIndices;
		TArray<FColor> Colors;
		TArray<FVector2f> UV0s, UV1s, UV2s, UV3s;
		UMoonToonEditorBPLibrary::MoonGetMeshData(Mesh, 0, Positions, VertexIndices, Normals,
			Tangents, Binormals, Colors, UV0s, UV1s, UV2s, UV3s);
		FMoonToonLODFaces Faces;
		MoonToonMesh::GetFaces(Mesh, 0, Faces);
		const FMoonToonWedgeMask Mask =
			MoonToonMesh::BuildWedgeMask(Faces, Context.SectionMaterialIndices, VertexIndices.Num());
		const FStrandEllipsoid Fit = ResolveStrandEllipsoid(*this, Positions, VertexIndices, Mask);

		// The actor's shell is the engine's 50cm-radius sphere, so scale = radii / 50.
		const FTransform LocalTransform(Fit.Rotation, Fit.Center, Fit.Radii / 50.0);
		const FTransform WorldTransform = LocalTransform * Component->GetComponentTransform();

		FActorSpawnParameters SpawnParameters;
		SpawnParameters.ObjectFlags = RF_Transient | RF_DuplicateTransient;
		AMoonToonStrandPreviewActor* PreviewActor = World->SpawnActor<AMoonToonStrandPreviewActor>(
			WorldTransform.GetLocation(), WorldTransform.Rotator(), SpawnParameters);
		if (!PreviewActor)
		{
			return TEXT("Could not spawn the preview actor.");
		}
		PreviewActor->SetActorScale3D(WorldTransform.GetScale3D());
		PreviewActor->InitializePreview(
			this, Component, Mesh, Context.SectionMaterialIndices, BandMaterial, ShellMaterial);

		if (GEditor)
		{
			GEditor->SelectNone(/*bNoteSelectionChange=*/false, /*bDeselectBSPSurfs=*/true);
			GEditor->SelectActor(PreviewActor, /*bInSelected=*/true, /*bNotify=*/true);
		}

		TArray<FString> PreviewLines;
		PreviewLines.Add(TEXT("Live preview started."));
		if (Meshes.Num() > 1)
		{
			PreviewLines.Add(TEXT("  Only the first target mesh gets the live preview."));
		}
		PreviewLines.Add(FString::Printf(TEXT("  Target: %s on '%s'"),
			*Mesh->GetName(), *Component->GetOwner()->GetActorLabel()));
		PreviewLines.Add(TEXT("  The selected ellipsoid actor IS the highlight shape: move / rotate / "
			"scale it with the ordinary gizmo and the band follows live."));
		PreviewLines.Add(TEXT("  Bake From Ellipsoid and Exit Preview are buttons on the actor's "
			"details panel; deleting the actor also restores the materials."));
		return FString::Join(PreviewLines, TEXT("\n"));
	}

	// --- Preview / Bake ---------------------------------------------------------------------------
	const bool bPreview = Mode == EMoonToonStrandTangentMode::Preview;

	// Refuse to write outside the project. Engine content is shared by every project on this
	// machine and is never a legitimate bake target; the live preview's ellipsoid shell IS
	// /Engine/BasicShapes/Sphere, so a stray retarget used to land the bake there.
	if (!bPreview)
	{
		for (int32 Index = Meshes.Num() - 1; Index >= 0; --Index)
		{
			const FString PackagePath = Meshes[Index]->GetPathName();
			if (PackagePath.StartsWith(TEXT("/Engine/")))
			{
				Lines.Add(FString::Printf(
					TEXT("  REFUSED %s: engine content is never a bake target (did the panel retarget "
						"onto the preview ellipsoid's sphere?)."), *PackagePath));
				Meshes.RemoveAt(Index);
			}
		}
		if (Meshes.Num() == 0)
		{
			Lines.Add(TEXT("Nothing left to bake."));
			return FString::Join(Lines, TEXT("\n"));
		}
	}
	Lines.Add(bPreview
		? TEXT("Preview Strand Directions  [nothing written]")
		: TEXT("Bake Strand Tangent"));
	if (!bPreview && Context.SectionMaterialIndices.Num() == 0)
	{
		Lines.Add(TEXT("  NOTE: no section filter. This writes UV1.xy / UV2.x across the whole mesh; "
			"the face-forward bake on a face section would be overwritten. Filter to the hair "
			"sections unless the mesh has no face."));
	}

	// One flush per run, before any mesh draws: each preview replaces the previous one.
	if (bPreview && GEditor)
	{
		if (UWorld* World = GEditor->GetEditorWorldContext().World())
		{
			FlushPersistentDebugLines(World);
		}
	}

	for (UObject* Mesh : Meshes)
	{
		if (!bPreview && bFixBuildSettingsFirst && UMoonToonSmoothNormalTool::FixBuildSettingsForMesh(Mesh))
		{
			Lines.Add(FString::Printf(TEXT("  %s: build settings were wrong, corrected and reimported."),
				*Mesh->GetName()));
		}

		const TArray<int32> LODs = ResolveStrandLODRange(Mesh, bAllLODs, LODIndex);
		if (LODs.Num() == 0)
		{
			Lines.Add(FString::Printf(TEXT("  %s: LOD %d does not exist -- skipped."), *Mesh->GetName(), LODIndex));
			continue;
		}

		// One rebuild for the whole LOD loop rather than one per LOD. Preview never writes, so it
		// never needs (or triggers) the rebuild.
		TUniquePtr<FMoonToonScopedMeshEdit> BatchedEdit;
		if (!bPreview)
		{
			BatchedEdit = MakeUnique<FMoonToonScopedMeshEdit>(Mesh);
		}

		for (int32 LOD : LODs)
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

			UMoonToonSmoothNormalTool::RecomputeMissingTangentAndBinormal(
				Positions, VertexIndices, UV0s, Normals, Tangents, Binormals);

			FMoonToonLODFaces Faces;
			MoonToonMesh::GetFaces(Mesh, LOD, Faces);

			const FMoonToonWedgeMask Mask =
				MoonToonMesh::BuildWedgeMask(Faces, Context.SectionMaterialIndices, NumWedges);

			const bool bEllipsoid = TangentSource == EMoonToonStrandTangentSource::Ellipsoid;
			FStrandEllipsoid Ellipsoid;
			FIslandAxisResult IslandAxes;
			if (bEllipsoid)
			{
				Ellipsoid = ResolveStrandEllipsoid(*this, Positions, VertexIndices, Mask);
				IslandAxes = ComputeEllipsoidField(Positions, VertexIndices, Mask, Ellipsoid);
			}
			else
			{
				IslandAxes = ComputeIslandAxes(Positions, VertexIndices, Faces, Mask, MinIslandVertices);
			}
			const FString SourceNote = bEllipsoid
				? FString::Printf(TEXT("ellipsoid centre %s radii %s rot %s"),
					*Ellipsoid.Center.ToCompactString(), *Ellipsoid.Radii.ToCompactString(),
					*EllipsoidRotation.ToCompactString())
				: FString::Printf(TEXT("%d island(s), %d skipped as too small"),
					IslandAxes.NumIslands, IslandAxes.NumSkippedIslands);

			// --- Preview: draw and move on. ----------------------------------------------------
			if (bPreview)
			{
				// Only the first LOD in the range: every LOD occupies the same space, and overdrawn
				// line sets are unreadable.
				if (LOD != LODs[0])
				{
					continue;
				}

				UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
				if (!World)
				{
					Lines.Add(TEXT("  No editor world -- cannot draw the preview."));
					continue;
				}

				const UMeshComponent* Placed = MoonToonMesh::FindPlacedMeshComponent(Mesh);
				const bool bPlaced = Placed != nullptr;
				const FTransform MeshToWorld = bPlaced ? Placed->GetComponentTransform() : FTransform::Identity;

				// Positions are the import reference pose: on an animated character the lines sit at
				// the ref pose, which is close enough to judge direction and island splits.
				const bool bPersistent = PreviewDurationSeconds <= 0.0f;
				const float LifeTime = bPersistent ? -1.0f : PreviewDurationSeconds;
				const int32 Stride = FMath::Max(1, PreviewVertexStride);

				// One representative wedge per vertex for the normal/tangent-plane projection.
				TArray<int32> VertexFirstWedge;
				VertexFirstWedge.Init(INDEX_NONE, Positions.Num());
				for (int32 WedgeIndex = 0; WedgeIndex < NumWedges; ++WedgeIndex)
				{
					const int32 VertexIndex = VertexIndices[WedgeIndex];
					if (Mask.Contains(WedgeIndex) && VertexFirstWedge.IsValidIndex(VertexIndex)
						&& VertexFirstWedge[VertexIndex] == INDEX_NONE)
					{
						VertexFirstWedge[VertexIndex] = WedgeIndex;
					}
				}

				int32 NumLinesDrawn = 0;
				for (int32 VertexIndex = 0; VertexIndex < Positions.Num(); VertexIndex += Stride)
				{
					const int32 WedgeIndex = VertexFirstWedge[VertexIndex];
					if (WedgeIndex == INDEX_NONE)
					{
						continue;
					}
					const FVector3f Axis = IslandAxes.PerVertexAxis[VertexIndex];
					if (Axis.IsNearlyZero())
					{
						continue;
					}

					const FVector3f Normal = Normals.IsValidIndex(WedgeIndex)
						? Normals[WedgeIndex] : FVector3f(0.0f, 0.0f, 1.0f);
					// The ellipsoid field is transferred as-is (its smoothness IS the feature);
					// only the per-card axis needs projecting onto the surface.
					const FVector3f StrandLocal = bEllipsoid ? Axis : ProjectAxisToTangentPlane(Axis, Normal);
					if (StrandLocal.IsNearlyZero())
					{
						continue;
					}

					// Lift the line slightly off the surface so it does not z-fight the mesh.
					const FVector LocalStart(Positions[VertexIndex] + Normal * 0.05f);
					const FVector WorldStart = MeshToWorld.TransformPosition(LocalStart);
					const FVector WorldDir = MeshToWorld.TransformVectorNoScale(FVector(StrandLocal)).GetSafeNormal();

					// Ellipsoid mode colours by direction (smoothness check); PCA colours by island
					// (segmentation check) -- each mode shows the thing that can go wrong with it.
					DrawDebugLine(World,
						WorldStart - WorldDir * (0.5f * PreviewLineLength),
						WorldStart + WorldDir * (0.5f * PreviewLineLength),
						bEllipsoid
							? StrandDirectionColor(StrandLocal)
							: StrandIslandColor(IslandAxes.PerVertexIsland[VertexIndex]),
						bPersistent, LifeTime, /*DepthPriority=*/0, /*Thickness=*/0.0f);
					++NumLinesDrawn;
				}

				if (bEllipsoid && bPreviewEllipsoidWireframe)
				{
					DrawEllipsoidWireframe(World, MeshToWorld, Ellipsoid, bPersistent, LifeTime);
				}

				Lines.Add(FString::Printf(
					TEXT("  %s LOD%d: %s, %d lines drawn%s%s."),
					*Mesh->GetName(), LOD, *SourceNote, NumLinesDrawn,
					bPlaced ? TEXT("") : TEXT(" AT WORLD ORIGIN (mesh is not placed in the level)"),
					bPersistent ? TEXT(", persistent until the next preview") : TEXT("")));
				continue;
			}

			// --- Bake. --------------------------------------------------------------------------
			BakeChannelsForLOD(Mesh, LOD, Mask, IslandAxes, /*bFieldAsIs=*/bEllipsoid, SourceNote,
				Positions, VertexIndices, Normals, Tangents, Binormals, Colors,
				UV0s, UV1s, UV2s, UV3s, Lines);
		}
	}

	Lines.Add(TEXT(""));
	Lines.Add(bPreview
		? TEXT("Preview only -- nothing was written. Switch Mode to Bake to commit these directions.")
		: TEXT("Enable the hair material's baked-strand-tangent switch to consume this. "
			"Mode = Restore returns the mesh to its pre-bake channels; reimporting also discards the bake."));
	return FString::Join(Lines, TEXT("\n"));
}

bool UMoonToonStrandTangentTool::IsLivePreviewActive()
{
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!World)
	{
		return false;
	}
	for (TActorIterator<AMoonToonStrandPreviewActor> It(World); It; ++It)
	{
		return true;
	}
	return false;
}

int32 UMoonToonStrandTangentTool::StopLivePreview()
{
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!World)
	{
		return 0;
	}

	TArray<AMoonToonStrandPreviewActor*> Actors;
	for (TActorIterator<AMoonToonStrandPreviewActor> It(World); It; ++It)
	{
		Actors.Add(*It);
	}
	for (AMoonToonStrandPreviewActor* Actor : Actors)
	{
		// Destroyed() puts the overridden hair materials back.
		Actor->Destroy();
	}
	return Actors.Num();
}

FString UMoonToonStrandTangentTool::BakeEllipsoidChannels(
	UObject* Mesh,
	const TArray<int32>& SectionMaterialIndices,
	const FVector& LocalCenter,
	const FVector& LocalRadii,
	const FQuat& LocalRotation,
	bool bAllLODs,
	int32 LODIndex)
{
	if (!Mesh)
	{
		return TEXT("No target mesh.");
	}
	if (Mesh->GetPathName().StartsWith(TEXT("/Engine/")))
	{
		return FString::Printf(TEXT("REFUSED: %s is engine content, never a bake target."),
			*Mesh->GetPathName());
	}

	TArray<FString> Lines;
	Lines.Add(TEXT("Bake Strand Tangent -- live-preview ellipsoid"));

	FStrandEllipsoid Ellipsoid;
	Ellipsoid.Center = LocalCenter;
	Ellipsoid.Radii = LocalRadii.ComponentMax(FVector(0.1));
	Ellipsoid.Rotation = LocalRotation;

	const TArray<int32> LODs = ResolveStrandLODRange(Mesh, bAllLODs, LODIndex);
	if (LODs.Num() == 0)
	{
		return FString::Printf(TEXT("%s: LOD %d does not exist."), *Mesh->GetName(), LODIndex);
	}

	FMoonToonScopedMeshEdit BatchedEdit(Mesh);
	for (int32 LOD : LODs)
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

		UMoonToonSmoothNormalTool::RecomputeMissingTangentAndBinormal(
			Positions, VertexIndices, UV0s, Normals, Tangents, Binormals);

		FMoonToonLODFaces Faces;
		MoonToonMesh::GetFaces(Mesh, LOD, Faces);
		const FMoonToonWedgeMask Mask =
			MoonToonMesh::BuildWedgeMask(Faces, SectionMaterialIndices, NumWedges);

		const FIslandAxisResult Axes = ComputeEllipsoidField(Positions, VertexIndices, Mask, Ellipsoid);
		const FString SourceNote = FString::Printf(TEXT("ellipsoid centre %s radii %s rot %s"),
			*Ellipsoid.Center.ToCompactString(), *Ellipsoid.Radii.ToCompactString(),
			*Ellipsoid.Rotation.Rotator().ToCompactString());

		BakeChannelsForLOD(Mesh, LOD, Mask, Axes, /*bFieldAsIs=*/true, SourceNote,
			Positions, VertexIndices, Normals, Tangents, Binormals, Colors,
			UV0s, UV1s, UV2s, UV3s, Lines);
	}

	Lines.Add(TEXT(""));
	Lines.Add(TEXT("Enable the hair material's baked-strand-tangent switch (with Use Anisotropy = 1) "
		"to consume this. The tool's Restore mode returns the mesh to its pre-bake channels."));
	return FString::Join(Lines, TEXT("\n"));
}

#undef LOCTEXT_NAMESPACE
