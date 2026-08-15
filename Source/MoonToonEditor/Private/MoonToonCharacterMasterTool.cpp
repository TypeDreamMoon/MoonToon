// Copyright Dream Moon. All Rights Reserved.

#include "MoonToonCharacterMasterTool.h"

#include "Materials/MaterialInstanceConstant.h"
#include "MoonToonMaterialLibrary.h"
#include "MoonToonMeshTargets.h"

#define LOCTEXT_NAMESPACE "MoonToonCharacterMasterTool"

namespace
{
	/** The character's name as an artist would say it: the mesh asset without its type prefix. */
	FString CharacterNameFromMesh(const UObject* Mesh)
	{
		FString Name = Mesh ? Mesh->GetName() : FString();
		for (const TCHAR* Prefix : { TEXT("SK_"), TEXT("SKM_"), TEXT("SM_") })
		{
			if (Name.StartsWith(Prefix))
			{
				return Name.RightChop(FCString::Strlen(Prefix));
			}
		}
		return Name;
	}
}

FText UMoonToonCharacterMasterTool::GetToolName() const
{
	return LOCTEXT("ToolName", "Character Master MI");
}

FText UMoonToonCharacterMasterTool::GetToolDescription() const
{
	return LOCTEXT("ToolDescription",
		"Inserts one material instance per character between its section instances and the shared "
		"MoonToon preset, so there is a single place to tune the whole character. Sections keep their "
		"own overrides; anything they all already agreed on moves up to the master. Nothing about the "
		"character's appearance changes.");
}

FText UMoonToonCharacterMasterTool::GetRunLabel() const
{
	return LOCTEXT("RunLabel", "Insert Character Master");
}

FString UMoonToonCharacterMasterTool::Run(const FMoonToonToolContext& Context)
{
	TArray<UObject*> Meshes;
	FString Report;
	if (!ResolveMeshes(Context, Meshes, Report))
	{
		return Report;
	}

	// One character per mesh: a master shared between two characters would be a fourth kind of
	// parent, not a character master. Meshes selected together each get their own.
	for (UObject* Mesh : Meshes)
	{
		TArray<FMoonToonSectionInfo> Sections;
		MoonToonMesh::GetSections(Mesh, 0, Sections);

		TArray<UMaterialInstanceConstant*> Instances;
		int32 NumNotInstances = 0;
		for (const FMoonToonSectionInfo& Section : Sections)
		{
			if (Context.SectionMaterialIndices.Num() > 0
				&& !Context.SectionMaterialIndices.Contains(Section.MaterialIndex))
			{
				continue;
			}

			UMaterialInterface* Material = Section.Material.Get();
			if (UMaterialInstanceConstant* Instance = Cast<UMaterialInstanceConstant>(Material))
			{
				Instances.AddUnique(Instance);
			}
			else if (Material)
			{
				// A plain UMaterial in a slot has no parent to insert under. Naming it matters: it is
				// how a section that was never converted to an instance shows up.
				++NumNotInstances;
			}
		}

		Report += FString::Printf(TEXT("=== %s ===\n"), *Mesh->GetName());
		if (Instances.Num() == 0)
		{
			Report += TEXT("  No material instances on this mesh's sections.\n\n");
			continue;
		}
		if (NumNotInstances > 0)
		{
			Report += FString::Printf(
				TEXT("  %d section(s) use a material rather than an instance and were left alone.\n"),
				NumNotInstances);
		}

		const FString Name = CharacterName.IsEmpty() ? CharacterNameFromMesh(Mesh) : CharacterName;
		Report += UMoonToonMaterialLibrary::InsertCharacterMasters(
			Instances,
			Name,
			DestinationFolder.Path,
			bPromoteCommonOverrides,
			bPromoteStaticSwitches);

		if (bSaveWhenDone)
		{
			// Save the children and, through their new parent link, the master with them.
			TArray<UMaterialInstanceConstant*> ToSave = Instances;
			for (UMaterialInstanceConstant* Instance : Instances)
			{
				if (UMaterialInstanceConstant* Master = Cast<UMaterialInstanceConstant>(Instance->Parent))
				{
					ToSave.AddUnique(Master);
				}
			}
			const int32 NumSaved = UMoonToonMaterialLibrary::SaveInstances(ToSave);
			Report += FString::Printf(TEXT("Saved %d of %d package(s).\n"), NumSaved, ToSave.Num());
		}
		Report += TEXT("\n");
	}

	return Report;
}

#undef LOCTEXT_NAMESPACE
