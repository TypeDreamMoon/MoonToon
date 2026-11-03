// Copyright Dream Moon. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MoonToonTool.h"
#include "MoonToonSectionMergeTool.generated.h"

class UMaterialInterface;
class UMeshComponent;
class USkeletalMesh;

/** One set of material slots whose sections become a single section. */
USTRUCT(BlueprintType)
struct MOONTOONEDITOR_API FMoonToonSectionMergeGroup
{
	GENERATED_BODY()

	/**
	 * Material slot indices. The FIRST is the slot the merged section keeps: its material, the outline and
	 * overlay entries indexed by it, and its section settings (cast shadow, ray tracing, recompute tangent).
	 * The other slots stay in the material list -- no slot index moves, so component overrides and the
	 * per-slot outline arrays keep lining up -- but no section uses them any more.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Merge")
	TArray<int32> MaterialSlots;

	/**
	 * The material the kept slot gets on the new mesh, e.g. the T22 sub-material instance that reproduces
	 * every merged slot through its table. Null keeps that slot's own material.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Merge")
	TObjectPtr<UMaterialInterface> Material = nullptr;
};

/** What a merge did. Counts are LOD 0 unless named otherwise. */
USTRUCT(BlueprintType)
struct MOONTOONEDITOR_API FMoonToonSectionMergeResult
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Merge")
	bool bSucceeded = false;

	/** The merged mesh (a new asset unless the source was overwritten in place). */
	UPROPERTY(BlueprintReadOnly, Category = "Merge")
	TObjectPtr<USkeletalMesh> Mesh = nullptr;

	UPROPERTY(BlueprintReadOnly, Category = "Merge")
	int32 SectionsBefore = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Merge")
	int32 SectionsAfter = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Merge")
	int32 VerticesBefore = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Merge")
	int32 VerticesAfter = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Merge")
	int32 MorphTargetsBefore = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Merge")
	int32 MorphTargetsAfter = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Merge")
	FString Report;
};

/** Where the groups come from when the tool runs from the panel. */
UENUM()
enum class EMoonToonSectionMergeSource : uint8
{
	/**
	 * Slots that show the SAME material asset on the mesh placed in the level (component overrides win) --
	 * what the T22 sub-material pilot leaves behind. The kept slot takes that material on the new mesh.
	 */
	PlacedComponentMaterials,

	/** Slots that already share a material asset on the mesh itself. */
	MeshMaterials,

	/** The groups listed below. */
	Explicit,
};

/**
 * Merges skeletal mesh sections that share a material into one section each -- fewer draws per character
 * (every section is a mesh batch in every pass it is drawn in: depth, base, velocity, shadows, the outline,
 * the toon prepasses).
 *
 * Works on the mesh description, not through a mesh round trip: the polygons of every merged slot move into
 * the kept slot's polygon group, and the rebuild forms one section per material slot. Vertices, UVs, skin
 * weights and the morph targets (vertex attributes of the mesh description) are untouched -- nothing is
 * re-imported, re-welded or re-ordered. Merged-away polygon groups stay behind empty, because the import
 * conversion indexes its material list by polygon group id; deleting a group would shift every later one.
 *
 * Non-destructive by default: it writes a NEW mesh beside the source (name + suffix). An existing destination,
 * or the source itself, is only overwritten after the panel asks (scripts pass bOverwriteExisting).
 *
 * Refused: meshes with clothing, and LODs that remap sections through a LOD material map.
 */
UCLASS()
class MOONTOONEDITOR_API UMoonToonSectionMergeTool : public UMoonToonTool
{
	GENERATED_BODY()

public:
	virtual FText GetToolName() const override;
	virtual FText GetToolDescription() const override;
	virtual FText GetRunLabel() const override;
	virtual FName GetToolIconName() const override { return TEXT("Icons.Duplicate"); }
	/** It writes a new asset; the source is only touched after an explicit confirmation. */
	virtual bool IsDestructive() const override { return false; }
	virtual FString Run(const FMoonToonToolContext& Context) override;

	UPROPERTY(EditAnywhere, Category = "Merge")
	EMoonToonSectionMergeSource GroupSource = EMoonToonSectionMergeSource::PlacedComponentMaterials;

	UPROPERTY(EditAnywhere, Category = "Merge", meta = (EditCondition = "GroupSource == EMoonToonSectionMergeSource::Explicit"))
	TArray<FMoonToonSectionMergeGroup> Groups;

	/** The new mesh is named <source><suffix>. Empty = overwrite the source in place (asks first). */
	UPROPERTY(EditAnywhere, Category = "Output")
	FString NameSuffix = TEXT("_Merged");

	/** Folder for the new mesh, e.g. /Game/Characters/Foo. Empty = beside the source. */
	UPROPERTY(EditAnywhere, Category = "Output")
	FString OutputFolder;

	/** Save the result to disk. Off leaves it dirty in memory for a look first. */
	UPROPERTY(EditAnywhere, Category = "Output")
	bool bSaveResult = true;

	/**
	 * Merges the given slot groups of Source into DestinationAssetPath ("/Game/Folder/Name"). Scripts: an
	 * existing destination (or the source's own path) is refused unless bOverwriteExisting.
	 */
	UFUNCTION(BlueprintCallable, Category = "MoonToon|Mesh")
	static FMoonToonSectionMergeResult MergeSkeletalMeshSections(USkeletalMesh* Source, const TArray<FMoonToonSectionMergeGroup>& InGroups,
		const FString& DestinationAssetPath, bool bOverwriteExisting = false, bool bSave = true);

	/**
	 * Slots of Mesh that show the same material asset, as merge groups (lowest slot first). With a Component,
	 * its effective per-slot materials (overrides included) decide, and each group carries that material.
	 * Only slots that own a section in LOD 0 are considered.
	 */
	UFUNCTION(BlueprintCallable, Category = "MoonToon|Mesh")
	static TArray<FMoonToonSectionMergeGroup> FindSharedMaterialGroups(USkeletalMesh* Mesh, UMeshComponent* Component);
};
