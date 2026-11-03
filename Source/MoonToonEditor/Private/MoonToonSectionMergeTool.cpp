// Copyright Dream Moon. All Rights Reserved.

#include "MoonToonSectionMergeTool.h"

#include "AssetToolsModule.h"
#include "Components/MeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/SkinnedAssetCommon.h"
#include "FileHelpers.h"
#include "IAssetTools.h"
#include "Materials/MaterialInterface.h"
#include "MeshDescription.h"
#include "Misc/MessageDialog.h"
#include "Misc/PackageName.h"
#include "Modules/ModuleManager.h"
#include "MoonToonMeshTargets.h"
#include "ObjectTools.h"
#include "Rendering/SkeletalMeshLODModel.h"
#include "Rendering/SkeletalMeshModel.h"
#include "SkeletalMeshAttributes.h"
#include "SkeletalMeshTypes.h"
#include "SkinnedAssetCompiler.h"
#include "UObject/Package.h"

#define LOCTEXT_NAMESPACE "MoonToonSectionMergeTool"

namespace MoonToonSectionMerge
{
	static void WaitForBuild(USkeletalMesh* Mesh)
	{
		USkinnedAsset* Assets[] = { Mesh };
		FSkinnedAssetCompilingManager::Get().FinishCompilation(Assets);
	}

	/** Render sections of a LOD -- bone-limit chunks included, since each one is a draw of its own. */
	static int32 CountSections(const USkeletalMesh* Mesh, int32 LODIndex)
	{
		const FSkeletalMeshModel* Model = Mesh ? Mesh->GetImportedModel() : nullptr;
		return (Model && Model->LODModels.IsValidIndex(LODIndex)) ? Model->LODModels[LODIndex].Sections.Num() : 0;
	}

	static int32 CountVertices(const USkeletalMesh* Mesh, int32 LODIndex)
	{
		const FSkeletalMeshModel* Model = Mesh ? Mesh->GetImportedModel() : nullptr;
		return (Model && Model->LODModels.IsValidIndex(LODIndex)) ? static_cast<int32>(Model->LODModels[LODIndex].NumVertices) : 0;
	}

	/**
	 * The section settings a user can change per section (cast shadow, ray tracing, recompute tangent...),
	 * keyed by material slot. The model keys them by the section's ORIGINAL index, which the merge reshuffles,
	 * so they are carried across the rebuild by slot instead.
	 */
	static TMap<int32, FSkelMeshSourceSectionUserData> SettingsBySlot(const FSkeletalMeshLODModel& LODModel)
	{
		TMap<int32, FSkelMeshSourceSectionUserData> Out;
		for (const FSkelMeshSection& Section : LODModel.Sections)
		{
			if (Section.ChunkedParentSectionIndex != INDEX_NONE || Out.Contains(Section.MaterialIndex))
			{
				continue;
			}
			if (const FSkelMeshSourceSectionUserData* UserData = LODModel.UserSectionsData.Find(Section.OriginalDataSectionIndex))
			{
				Out.Add(Section.MaterialIndex, *UserData);
			}
		}
		return Out;
	}

	static bool SameSettings(const FSkelMeshSourceSectionUserData& A, const FSkelMeshSourceSectionUserData& B)
	{
		return A.bDisabled == B.bDisabled
			&& A.bCastShadow == B.bCastShadow
			&& A.bVisibleInRayTracing == B.bVisibleInRayTracing
			&& A.bRecomputeTangent == B.bRecomputeTangent
			&& A.RecomputeTangentsVertexMaskChannel == B.RecomputeTangentsVertexMaskChannel
			&& A.GenerateUpToLodIndex == B.GenerateUpToLodIndex;
	}

	/** "/Game/A/Name" or "/Game/A/Name.Name" -> package name + asset name. */
	static bool SplitDestination(const FString& InPath, FString& OutPackageName, FString& OutAssetName, FString& OutError)
	{
		FString PackageName = InPath.TrimStartAndEnd();
		int32 Dot = INDEX_NONE;
		if (PackageName.FindChar(TEXT('.'), Dot))
		{
			PackageName.LeftInline(Dot);
		}
		FText Reason;
		if (!FPackageName::IsValidLongPackageName(PackageName, false, &Reason))
		{
			OutError = FString::Printf(TEXT("'%s' is not a valid asset path (%s)"), *InPath, *Reason.ToString());
			return false;
		}
		OutPackageName = PackageName;
		OutAssetName = FPackageName::GetLongPackageAssetName(PackageName);
		return true;
	}

	static FString SlotLabel(const USkeletalMesh* Mesh, int32 Slot)
	{
		const TArray<FSkeletalMaterial>& Materials = Mesh->GetMaterials();
		if (!Materials.IsValidIndex(Slot))
		{
			return FString::Printf(TEXT("%d"), Slot);
		}
		const UMaterialInterface* Material = Materials[Slot].MaterialInterface;
		return FString::Printf(TEXT("%d %s (%s)"), Slot, *Materials[Slot].MaterialSlotName.ToString(),
			Material ? *Material->GetName() : TEXT("none"));
	}
}

FText UMoonToonSectionMergeTool::GetToolName() const
{
	return LOCTEXT("SectionMergeName", "Merge Sections");
}

FText UMoonToonSectionMergeTool::GetToolDescription() const
{
	return LOCTEXT("SectionMergeDesc",
		"Merges skeletal mesh sections that share a material into one section each, for fewer draws per character. "
		"Writes a NEW mesh beside the source (name + suffix); vertices, UVs, skin weights and morph targets are kept as they are. "
		"Groups: slots that show the same material on the mesh placed in the level (after the sub-material table pilot), "
		"slots that share a material on the mesh itself, or an explicit list.");
}

FText UMoonToonSectionMergeTool::GetRunLabel() const
{
	return LOCTEXT("SectionMergeRun", "Merge into New Mesh");
}

FMoonToonSectionMergeResult UMoonToonSectionMergeTool::MergeSkeletalMeshSections(USkeletalMesh* Source,
	const TArray<FMoonToonSectionMergeGroup>& InGroups, const FString& DestinationAssetPath, bool bOverwriteExisting, bool bSave)
{
	using namespace MoonToonSectionMerge;

	FMoonToonSectionMergeResult Result;
	TArray<FString> Lines;
	auto Finish = [&Result, &Lines](bool bOk, const FString& Last = FString())
	{
		if (!Last.IsEmpty())
		{
			Lines.Add(bOk ? Last : TEXT("FAILED: ") + Last);
		}
		Result.bSucceeded = bOk;
		Result.Report = FString::Join(Lines, TEXT("\n"));
		return Result;
	};

	if (!Source)
	{
		return Finish(false, TEXT("no source mesh"));
	}
	WaitForBuild(Source);
	Lines.Add(FString::Printf(TEXT("%s -> %s"), *Source->GetPathName(), *DestinationAssetPath));

	const FSkeletalMeshModel* SourceModel = Source->GetImportedModel();
	if (!SourceModel || SourceModel->LODModels.Num() == 0)
	{
		return Finish(false, TEXT("the source has no imported model"));
	}

	// --- The groups: each slot in one group at most; the first slot of a group is the one kept. ---
	const int32 NumSlots = Source->GetMaterials().Num();
	TMap<int32, int32> SlotToKept;
	TSet<int32> UsedSlots;
	TArray<FMoonToonSectionMergeGroup> ValidGroups;
	for (const FMoonToonSectionMergeGroup& Group : InGroups)
	{
		if (Group.MaterialSlots.Num() < 2)
		{
			continue;
		}
		for (const int32 Slot : Group.MaterialSlots)
		{
			if (Slot < 0 || Slot >= NumSlots)
			{
				return Finish(false, FString::Printf(TEXT("slot %d does not exist (the mesh has %d)"), Slot, NumSlots));
			}
			if (UsedSlots.Contains(Slot))
			{
				return Finish(false, FString::Printf(TEXT("slot %d is in two groups"), Slot));
			}
			UsedSlots.Add(Slot);
		}
		for (int32 Index = 1; Index < Group.MaterialSlots.Num(); ++Index)
		{
			SlotToKept.Add(Group.MaterialSlots[Index], Group.MaterialSlots[0]);
		}
		ValidGroups.Add(Group);
	}
	if (SlotToKept.Num() == 0)
	{
		return Finish(false, TEXT("no group has two or more slots: nothing to merge"));
	}

	// --- What the merge cannot carry. ---
	if (Source->GetMeshClothingAssets().Num() > 0)
	{
		return Finish(false, TEXT("the mesh has clothing; re-binding cloth to merged sections is not supported"));
	}
	for (int32 LODIndex = 0; LODIndex < Source->GetLODNum(); ++LODIndex)
	{
		const FSkeletalMeshLODInfo* LODInfo = Source->GetLODInfo(LODIndex);
		if (!LODInfo || !SourceModel->LODModels.IsValidIndex(LODIndex))
		{
			continue;
		}
		const FSkeletalMeshLODModel& LODModel = SourceModel->LODModels[LODIndex];
		for (int32 SectionIndex = 0; SectionIndex < LODInfo->LODMaterialMap.Num(); ++SectionIndex)
		{
			const int32 Mapped = LODInfo->LODMaterialMap[SectionIndex];
			if (Mapped != INDEX_NONE && (!LODModel.Sections.IsValidIndex(SectionIndex) || LODModel.Sections[SectionIndex].MaterialIndex != Mapped))
			{
				return Finish(false, FString::Printf(TEXT("LOD %d remaps section %d to slot %d through its LOD material map; not supported"),
					LODIndex, SectionIndex, Mapped));
			}
		}
	}

	// --- The destination: a new asset, or (asked for) an existing one / the source itself. ---
	FString PackageName, AssetName, Error;
	if (!SplitDestination(DestinationAssetPath, PackageName, AssetName, Error))
	{
		return Finish(false, Error);
	}

	Result.SectionsBefore = CountSections(Source, 0);
	Result.VerticesBefore = CountVertices(Source, 0);
	Result.MorphTargetsBefore = Source->GetMorphTargets().Num();

	USkeletalMesh* Mesh = nullptr;
	if (PackageName == Source->GetPackage()->GetName())
	{
		if (!bOverwriteExisting)
		{
			return Finish(false, TEXT("the destination is the source itself; pass bOverwriteExisting to merge in place"));
		}
		Mesh = Source;
		Lines.Add(TEXT("in place: the source mesh itself is changed"));
	}
	else
	{
		if (UPackage* ExistingPackage = FindPackage(nullptr, *PackageName); ExistingPackage || FPackageName::DoesPackageExist(PackageName))
		{
			if (!bOverwriteExisting)
			{
				return Finish(false, FString::Printf(TEXT("%s already exists; pass bOverwriteExisting to replace it"), *PackageName));
			}
			if (UObject* Existing = LoadObject<UObject>(nullptr, *(PackageName + TEXT(".") + AssetName), nullptr, LOAD_NoWarn | LOAD_Quiet))
			{
				if (ObjectTools::ForceDeleteObjects({ Existing }, false) == 0)
				{
					return Finish(false, FString::Printf(TEXT("could not delete the existing %s"), *PackageName));
				}
				Lines.Add(FString::Printf(TEXT("replaced the existing %s"), *PackageName));
			}
		}

		IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
		Mesh = Cast<USkeletalMesh>(AssetTools.DuplicateAsset(AssetName, FPackageName::GetLongPackagePath(PackageName), Source));
		if (!Mesh)
		{
			return Finish(false, TEXT("duplicating the source failed"));
		}
		WaitForBuild(Mesh);
	}
	Result.Mesh = Mesh;

	FSkeletalMeshModel* Model = Mesh->GetImportedModel();
	const int32 NumLODs = Mesh->GetLODNum();

	// Section settings by slot, from before the merge, and a note where a group's slots disagree.
	TArray<TMap<int32, FSkelMeshSourceSectionUserData>> SavedSettings;
	SavedSettings.SetNum(NumLODs);
	for (int32 LODIndex = 0; LODIndex < NumLODs; ++LODIndex)
	{
		if (Model->LODModels.IsValidIndex(LODIndex))
		{
			SavedSettings[LODIndex] = SettingsBySlot(Model->LODModels[LODIndex]);
		}
	}
	{
		TArray<TObjectPtr<UMaterialInterface>> OutlineSlots;
		Mesh->GetMoonOutlineMaterialSlots(OutlineSlots);
		for (const FMoonToonSectionMergeGroup& Group : ValidGroups)
		{
			const int32 Kept = Group.MaterialSlots[0];
			TArray<FString> Merged;
			for (int32 Index = 1; Index < Group.MaterialSlots.Num(); ++Index)
			{
				const int32 Slot = Group.MaterialSlots[Index];
				Merged.Add(SlotLabel(Mesh, Slot));
				const FSkelMeshSourceSectionUserData* A = SavedSettings[0].Find(Kept);
				const FSkelMeshSourceSectionUserData* B = SavedSettings[0].Find(Slot);
				if (A && B && !SameSettings(*A, *B))
				{
					Lines.Add(FString::Printf(TEXT("  note: slot %d's section settings differ from slot %d's; the merged section takes slot %d's"),
						Slot, Kept, Kept));
				}
				const UMaterialInterface* KeptOutline = OutlineSlots.IsValidIndex(Kept) ? OutlineSlots[Kept].Get() : nullptr;
				const UMaterialInterface* SlotOutline = OutlineSlots.IsValidIndex(Slot) ? OutlineSlots[Slot].Get() : nullptr;
				if (KeptOutline != SlotOutline)
				{
					Lines.Add(FString::Printf(TEXT("  note: slot %d's outline material (%s) differs from slot %d's (%s); the merged section draws slot %d's"),
						Slot, SlotOutline ? *SlotOutline->GetName() : TEXT("none"), Kept, KeptOutline ? *KeptOutline->GetName() : TEXT("none"), Kept));
				}
			}
			Lines.Add(FString::Printf(TEXT("  keep %s  <- %s%s"), *SlotLabel(Mesh, Kept), *FString::Join(Merged, TEXT(", ")),
				Group.Material ? *FString::Printf(TEXT("  [material -> %s]"), *Group.Material->GetName()) : TEXT("")));
		}
	}

	// --- The merge: move every merged slot's polygons into the kept slot's polygon group. ---
	{
		FScopedSkeletalMeshPostEditChange ScopedPostEditChange(Mesh);
		Mesh->Modify();
		TArray<FSkeletalMaterial>& Materials = Mesh->GetMaterials();

		for (int32 LODIndex = 0; LODIndex < NumLODs; ++LODIndex)
		{
			if (!Mesh->HasMeshDescription(LODIndex))
			{
				Lines.Add(FString::Printf(TEXT("  LOD %d: no mesh description (a generated LOD) -- rebuilt from its source LOD"), LODIndex));
				continue;
			}
			FMeshDescription* MeshDescription = Mesh->GetMeshDescription(LODIndex);
			if (!MeshDescription)
			{
				continue;
			}
			FSkeletalMeshAttributes Attributes(*MeshDescription);
			TPolygonGroupAttributesRef<FName> SlotNames = Attributes.GetPolygonGroupMaterialSlotNames();

			// Which slot a polygon group draws with, the way the build decides it: LOD 0 matches the imported
			// slot name against the mesh's slots; other LODs use the group index (no LOD material map here).
			auto SlotOfGroup = [&](const FPolygonGroupID GroupID)
			{
				if (LODIndex == 0)
				{
					const FName SlotName = SlotNames[GroupID];
					const int32 ByName = Materials.IndexOfByPredicate([SlotName](const FSkeletalMaterial& Material)
						{ return Material.ImportedMaterialSlotName == SlotName; });
					if (ByName != INDEX_NONE)
					{
						return ByName;
					}
				}
				return GroupID.GetValue() < Materials.Num() ? GroupID.GetValue() : INDEX_NONE;
			};

			TMap<int32, FPolygonGroupID> GroupOfSlot;
			TArray<TPair<FPolygonGroupID, int32>> GroupSlots;
			for (const FPolygonGroupID GroupID : MeshDescription->PolygonGroups().GetElementIDs())
			{
				const int32 Slot = SlotOfGroup(GroupID);
				GroupSlots.Emplace(GroupID, Slot);
				if (Slot != INDEX_NONE && MeshDescription->GetNumPolygonGroupPolygons(GroupID) > 0 && !GroupOfSlot.Contains(Slot))
				{
					GroupOfSlot.Add(Slot, GroupID);
				}
			}

			int32 MovedPolygons = 0;
			int32 MergedGroups = 0;
			for (const TPair<FPolygonGroupID, int32>& GroupSlot : GroupSlots)
			{
				const int32* Kept = SlotToKept.Find(GroupSlot.Value);
				if (!Kept || MeshDescription->GetNumPolygonGroupPolygons(GroupSlot.Key) == 0)
				{
					continue;
				}
				const FPolygonGroupID* Target = GroupOfSlot.Find(*Kept);
				if (!Target)
				{
					// The kept slot has no geometry in this LOD. On LOD 0 the first merged group takes its name
					// and becomes the target; elsewhere the slot is the group index, so it stays separate.
					if (LODIndex == 0)
					{
						SlotNames[GroupSlot.Key] = Materials[*Kept].ImportedMaterialSlotName;
						GroupOfSlot.Add(*Kept, GroupSlot.Key);
						++MergedGroups;
					}
					else
					{
						Lines.Add(FString::Printf(TEXT("  LOD %d: slot %d has no geometry here, slot %d stays a section of its own"),
							LODIndex, *Kept, GroupSlot.Value));
					}
					continue;
				}
				// Copy first: moving a polygon edits the very array GetPolygonGroupPolygonIDs views.
				const TArray<FPolygonID> Polygons(MeshDescription->GetPolygonGroupPolygonIDs(GroupSlot.Key));
				for (const FPolygonID PolygonID : Polygons)
				{
					MeshDescription->SetPolygonPolygonGroup(PolygonID, *Target);
				}
				MovedPolygons += Polygons.Num();
				++MergedGroups;
			}
			Mesh->CommitMeshDescription(LODIndex);
			Lines.Add(FString::Printf(TEXT("  LOD %d: %d slot(s) merged, %d polygons moved"), LODIndex, MergedGroups, MovedPolygons));

			// Sections are about to be renumbered; an identity / empty LOD material map (all that was accepted
			// above) carries nothing, and a stale one would point new sections at old slots.
			if (FSkeletalMeshLODInfo* LODInfo = Mesh->GetLODInfo(LODIndex))
			{
				LODInfo->LODMaterialMap.Reset();
			}
		}

		for (const FMoonToonSectionMergeGroup& Group : ValidGroups)
		{
			if (Group.Material)
			{
				Materials[Group.MaterialSlots[0]].MaterialInterface = Group.Material;
			}
		}
	}
	WaitForBuild(Mesh);

	// --- Section settings back onto the merged sections, by slot. ---
	bool bSettingsRekeyed = false;
	for (int32 LODIndex = 0; LODIndex < NumLODs && LODIndex < Model->LODModels.Num(); ++LODIndex)
	{
		FSkeletalMeshLODModel& LODModel = Model->LODModels[LODIndex];
		TMap<int32, FSkelMeshSourceSectionUserData> Rekeyed;
		for (const FSkelMeshSection& Section : LODModel.Sections)
		{
			if (Section.ChunkedParentSectionIndex != INDEX_NONE)
			{
				continue;
			}
			if (const FSkelMeshSourceSectionUserData* Wanted = SavedSettings[LODIndex].Find(Section.MaterialIndex))
			{
				Rekeyed.Add(Section.OriginalDataSectionIndex, *Wanted);
			}
			else if (const FSkelMeshSourceSectionUserData* Current = LODModel.UserSectionsData.Find(Section.OriginalDataSectionIndex))
			{
				Rekeyed.Add(Section.OriginalDataSectionIndex, *Current);
			}
		}
		bool bSame = Rekeyed.Num() == LODModel.UserSectionsData.Num();
		for (const TPair<int32, FSkelMeshSourceSectionUserData>& Pair : Rekeyed)
		{
			const FSkelMeshSourceSectionUserData* Current = LODModel.UserSectionsData.Find(Pair.Key);
			bSame = bSame && Current && SameSettings(*Current, Pair.Value);
		}
		if (!bSame)
		{
			LODModel.UserSectionsData = MoveTemp(Rekeyed);
			LODModel.SyncronizeUserSectionsDataArray();
			bSettingsRekeyed = true;
		}
	}
	if (bSettingsRekeyed)
	{
		{
			FScopedSkeletalMeshPostEditChange ScopedPostEditChange(Mesh);
		}
		WaitForBuild(Mesh);
		Lines.Add(TEXT("  section settings re-keyed onto the merged sections"));
	}

	Result.SectionsAfter = CountSections(Mesh, 0);
	Result.VerticesAfter = CountVertices(Mesh, 0);
	Result.MorphTargetsAfter = Mesh->GetMorphTargets().Num();
	Lines.Add(FString::Printf(TEXT("  LOD 0 sections %d -> %d (%d fewer mesh batches in every pass the character is drawn in), vertices %d -> %d, morph targets %d -> %d"),
		Result.SectionsBefore, Result.SectionsAfter, Result.SectionsBefore - Result.SectionsAfter,
		Result.VerticesBefore, Result.VerticesAfter, Result.MorphTargetsBefore, Result.MorphTargetsAfter));
	if (Result.MorphTargetsAfter != Result.MorphTargetsBefore)
	{
		Lines.Add(TEXT("  WARNING: the morph target count changed"));
	}

	Mesh->MarkPackageDirty();
	if (bSave)
	{
		const bool bSaved = UEditorLoadingAndSavingUtils::SavePackages({ Mesh->GetPackage() }, false);
		Lines.Add(bSaved ? TEXT("  saved") : TEXT("  NOT saved (the save failed); the mesh is dirty in memory"));
	}
	return Finish(true);
}

TArray<FMoonToonSectionMergeGroup> UMoonToonSectionMergeTool::FindSharedMaterialGroups(USkeletalMesh* Mesh, UMeshComponent* Component)
{
	TArray<FMoonToonSectionMergeGroup> Out;
	if (!Mesh)
	{
		return Out;
	}
	MoonToonSectionMerge::WaitForBuild(Mesh);

	TSet<int32> SlotsWithSections;
	if (const FSkeletalMeshModel* Model = Mesh->GetImportedModel(); Model && Model->LODModels.Num() > 0)
	{
		for (const FSkelMeshSection& Section : Model->LODModels[0].Sections)
		{
			SlotsWithSections.Add(Section.MaterialIndex);
		}
	}

	const TArray<FSkeletalMaterial>& Materials = Mesh->GetMaterials();
	TMap<UMaterialInterface*, int32> GroupOfMaterial;
	for (int32 Slot = 0; Slot < Materials.Num(); ++Slot)
	{
		if (!SlotsWithSections.Contains(Slot))
		{
			continue;
		}
		UMaterialInterface* Material = Component ? Component->GetMaterial(Slot) : Materials[Slot].MaterialInterface.Get();
		if (!Material)
		{
			continue;
		}
		if (const int32* GroupIndex = GroupOfMaterial.Find(Material))
		{
			Out[*GroupIndex].MaterialSlots.Add(Slot);
		}
		else
		{
			FMoonToonSectionMergeGroup Group;
			Group.MaterialSlots.Add(Slot);
			// From a component the shared material may be an override the mesh does not carry; the kept slot
			// takes it on the new mesh. From the mesh it is already there.
			Group.Material = (Component && Material != Materials[Slot].MaterialInterface) ? Material : nullptr;
			GroupOfMaterial.Add(Material, Out.Add(Group));
		}
	}
	Out.RemoveAll([](const FMoonToonSectionMergeGroup& Group) { return Group.MaterialSlots.Num() < 2; });
	return Out;
}

FString UMoonToonSectionMergeTool::Run(const FMoonToonToolContext& Context)
{
	TArray<UObject*> Meshes;
	FString Report;
	if (!ResolveMeshes(Context, Meshes, Report))
	{
		return Report;
	}

	TArray<FString> Lines;
	for (UObject* Object : Meshes)
	{
		USkeletalMesh* Source = Cast<USkeletalMesh>(Object);
		if (!Source)
		{
			Lines.Add(FString::Printf(TEXT("%s: not a skeletal mesh, skipped"), *Object->GetName()));
			continue;
		}

		TArray<FMoonToonSectionMergeGroup> UseGroups;
		switch (GroupSource)
		{
		case EMoonToonSectionMergeSource::PlacedComponentMaterials:
			if (UMeshComponent* Component = MoonToonMesh::FindPlacedMeshComponent(Source))
			{
				UseGroups = FindSharedMaterialGroups(Source, Component);
			}
			else
			{
				Lines.Add(FString::Printf(TEXT("%s: not placed in the level, so there are no component materials to group by"), *Source->GetName()));
				continue;
			}
			break;
		case EMoonToonSectionMergeSource::MeshMaterials:
			UseGroups = FindSharedMaterialGroups(Source, nullptr);
			break;
		default:
			UseGroups = Groups;
			break;
		}
		if (UseGroups.Num() == 0)
		{
			Lines.Add(FString::Printf(TEXT("%s: no two sections share a material; nothing to merge"), *Source->GetName()));
			continue;
		}

		const FString SourcePackage = Source->GetPackage()->GetName();
		const FString Folder = OutputFolder.IsEmpty() ? FPackageName::GetLongPackagePath(SourcePackage) : OutputFolder;
		const FString Destination = Folder / (Source->GetName() + NameSuffix);
		const bool bInPlace = Destination == SourcePackage;
		bool bOverwrite = false;
		if (bInPlace || FindPackage(nullptr, *Destination) || FPackageName::DoesPackageExist(Destination))
		{
			const FText Question = bInPlace
				? FText::Format(LOCTEXT("MergeInPlace", "Merge the sections of {0} IN PLACE? This changes the source mesh itself."),
					FText::FromString(SourcePackage))
				: FText::Format(LOCTEXT("MergeOverwrite", "{0} already exists. Replace it? Anything that references it loses the reference."),
					FText::FromString(Destination));
			if (FMessageDialog::Open(EAppMsgType::YesNo, Question) != EAppReturnType::Yes)
			{
				Lines.Add(FString::Printf(TEXT("%s: skipped, %s was not overwritten"), *Source->GetName(), *Destination));
				continue;
			}
			bOverwrite = true;
		}

		const FMoonToonSectionMergeResult Result = MergeSkeletalMeshSections(Source, UseGroups, Destination, bOverwrite, bSaveResult);
		Lines.Add(Result.Report);
	}
	return FString::Join(Lines, TEXT("\n"));
}

#undef LOCTEXT_NAMESPACE
