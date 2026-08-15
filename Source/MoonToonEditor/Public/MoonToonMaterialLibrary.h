// Copyright Dream Moon. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Materials/MaterialParameters.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "MoonToonMaterialLibrary.generated.h"

class UMaterialInstanceConstant;
class UMaterialInterface;

/**
 * Batch operations on material instances, and the parameter plumbing the tools panel edits with.
 *
 * Everything here is deliberately independent of Slate: the panel's parameter list, the Content
 * Browser context menu and Python all go through the same functions, so a batch re-parent behaves
 * identically however it was started -- and can be regression-tested from a script.
 */

/** One parameter as the panel sees it across the whole selection of instances. */
struct FMoonToonMaterialParam
{
	FMaterialParameterInfo Info;
	EMaterialParameterType Type = EMaterialParameterType::None;

	FName Group;
	int32 SortPriority = 0;
	FText Description;

	/** Slider bounds from the parameter's authoring metadata. Equal when it declared none. */
	float ScalarMin = 0.0f;
	float ScalarMax = 0.0f;

	/** Authored as a channel mask, so a colour picker is the wrong editor for it. */
	bool bUsedAsChannelMask = false;

	/** The value every selected instance agrees on. Meaningless unless bSameValue. */
	FMaterialParameterValue Value;
	bool bSameValue = true;

	/** How many of the selected instances override it, out of how many were asked. */
	int32 NumOverriding = 0;
	int32 NumInstances = 0;

	bool IsOverriddenByAll() const { return NumInstances > 0 && NumOverriding == NumInstances; }
	bool IsOverriddenBySome() const { return NumOverriding > 0 && NumOverriding < NumInstances; }
};

namespace MoonToonMaterial
{
	/**
	 * Parameter types the panel edits. Fonts, virtual textures, collections and material layers are
	 * left to the real material instance editor: they are rare on a character and each needs its own
	 * picker widget to be worth showing.
	 */
	MOONTOONEDITOR_API TArrayView<const EMaterialParameterType> GetEditableTypes();

	/** True when this instance -- not one of its parents -- carries a value for the parameter. */
	MOONTOONEDITOR_API bool IsOverriddenHere(
		UMaterialInstanceConstant* Instance,
		EMaterialParameterType Type,
		const FMaterialParameterInfo& Info);

	/** True when the parent chain declares the parameter at all. False means a stale override. */
	MOONTOONEDITOR_API bool ExistsInParent(
		UMaterialInstanceConstant* Instance,
		EMaterialParameterType Type,
		const FMaterialParameterInfo& Info);

	/** Type-aware value comparison, for deciding whether a multi-selection shares a value. */
	MOONTOONEDITOR_API bool ValuesEqual(const FMaterialParameterValue& A, const FMaterialParameterValue& B);

	/**
	 * The parameters common to every instance, merged into one row each.
	 *
	 * Common rather than the union: a row that only some of the selection has cannot be edited as one
	 * value. OutSkippedCount reports how many were dropped that way, which the panel shows.
	 */
	MOONTOONEDITOR_API void GatherSharedParameters(
		const TArray<UMaterialInstanceConstant*>& Instances,
		TArray<FMoonToonMaterialParam>& OutParams,
		int32& OutSkippedCount);

	/**
	 * Writes a value onto one instance, creating the override if it did not have one.
	 *
	 * Does NOT publish the change -- call FinishEdits once for the whole batch. Writing N parameters
	 * across M instances otherwise pays M*N asset rebuilds, each of which re-checks the static
	 * permutation and redraws every viewport.
	 */
	MOONTOONEDITOR_API bool ApplyValue(
		UMaterialInstanceConstant* Instance,
		EMaterialParameterType Type,
		const FMaterialParameterInfo& Info,
		const FMaterialParameterValue& Value);

	/** Drops this instance's override, so the parameter falls back to the parent. Also deferred. */
	MOONTOONEDITOR_API bool ClearOverride(
		UMaterialInstanceConstant* Instance,
		EMaterialParameterType Type,
		const FMaterialParameterInfo& Info);

	/** Publishes deferred edits: dirty the package, rebuild, recompile static permutations, redraw. */
	MOONTOONEDITOR_API void FinishEdits(const TArray<UMaterialInstanceConstant*>& Instances);

	/** Cheap in-viewport refresh for a value being dragged, without dirtying or recompiling. */
	MOONTOONEDITOR_API void PreviewEdits(const TArray<UMaterialInstanceConstant*>& Instances);

	DECLARE_MULTICAST_DELEGATE_OneParam(FOnReport, const FString& /*Report*/);

	/**
	 * Where a batch operation's report goes.
	 *
	 * The Content Browser has nowhere to print several paragraphs, and these reports are the only
	 * record of which overrides a re-parent carried over. The tools panel listens and shows them in
	 * its output pane; with no listener they still reach the log.
	 */
	MOONTOONEDITOR_API FOnReport& OnReport();
}

UCLASS()
class MOONTOONEDITOR_API UMoonToonMaterialLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	/**
	 * Re-parents every instance, keeping the overrides whose parameter still exists.
	 *
	 * Overrides are stored by parameter name, so they survive a re-parent whenever the new parent
	 * declares the same name -- which is the whole point of swapping a base material. The ones it
	 * does not declare become dead weight that still serialises: bClearOrphanedOverrides removes
	 * them, and either way the report names them.
	 */
	UFUNCTION(BlueprintCallable, Category = "MoonToon|Material")
	static FString SetParentOnInstances(
		const TArray<UMaterialInstanceConstant*>& Instances,
		UMaterialInterface* NewParent,
		bool bClearOrphanedOverrides = true);

	/**
	 * Inserts a per-character master instance between the given instances and their parent.
	 *
	 * A VRM import parents every section's instance straight at the shared MoonToon preset, so there
	 * is no place to put a value that means "this character". This carves one out:
	 *
	 *     M_MoonToon -> MI_Moon_Toon_VRM_TwoSide -> MI_<Character>_Master -> MI_<section>
	 *
	 * Instances are grouped by the parent they currently have, and each group gets its own master --
	 * a Masked section cannot hang off a TwoSide preset without losing its blend mode, so one master
	 * per character only works when the character uses one preset, and the grouping makes that the
	 * automatic case rather than a special one. With several, the master names are suffixed with
	 * whatever distinguishes their presets.
	 *
	 * The masters start empty, so the character renders identically until something is edited on one.
	 * bPromoteCommonOverrides is what makes them immediately useful: an override every instance in the
	 * group carries with the same value is a character-wide decision, so it moves up to the master and
	 * out of the children. Static switches are excluded unless bPromoteStaticSwitches, because
	 * clearing one has to go through the engine's own entry point, which republishes and recompiles
	 * the instance on every single call.
	 *
	 * CharacterName is the bare name (no MI_ prefix). DestinationFolder is a content path such as
	 * "/Game/Characters/Lin"; empty means alongside the instances themselves.
	 */
	UFUNCTION(BlueprintCallable, Category = "MoonToon|Material")
	static FString InsertCharacterMasters(
		const TArray<UMaterialInstanceConstant*>& Instances,
		const FString& CharacterName,
		const FString& DestinationFolder,
		bool bPromoteCommonOverrides = true,
		bool bPromoteStaticSwitches = false);

	/**
	 * Points the given material slots of a mesh at one material.
	 *
	 * MaterialIndices are slot indices, the same identity the tools panel filters sections by; an
	 * empty array means every slot. Transacted, because unlike the mesh bakes this one really is
	 * undoable -- it is a property on the asset, not a rebuild of its import data.
	 */
	UFUNCTION(BlueprintCallable, Category = "MoonToon|Material")
	static FString AssignMaterialToMeshSections(
		UObject* Mesh,
		const TArray<int32>& MaterialIndices,
		UMaterialInterface* Material);

	/**
	 * Creates one material instance per slot, named after the slot, and assigns it.
	 *
	 * The other half of a VRM cleanup: sections that came in pointing at a plain material, or at
	 * something shared, each need an instance of their own before they can be tuned separately.
	 * Slots already using an instance are left alone unless bReplaceExistingInstances.
	 */
	UFUNCTION(BlueprintCallable, Category = "MoonToon|Material")
	static FString CreateInstancesForMeshSections(
		UObject* Mesh,
		const TArray<int32>& MaterialIndices,
		UMaterialInterface* Parent,
		const FString& DestinationFolder,
		bool bReplaceExistingInstances = false);

	/** Removes overrides whose parameter no longer exists anywhere in the parent chain. */
	UFUNCTION(BlueprintCallable, Category = "MoonToon|Material")
	static FString ClearOrphanedOverrides(const TArray<UMaterialInstanceConstant*>& Instances);

	/**
	 * Copies Source's overrides onto every target, skipping parameters the target's parent does not
	 * declare. Existing target overrides the source does not carry are left alone.
	 */
	UFUNCTION(BlueprintCallable, Category = "MoonToon|Material")
	static FString CopyOverrides(
		UMaterialInstanceConstant* Source,
		const TArray<UMaterialInstanceConstant*>& Targets);

	/**
	 * Sets one scalar on every instance -- the same write the panel's parameter row performs.
	 *
	 * Instances whose parent does not declare the parameter are skipped rather than given a dead
	 * override, and the report says which.
	 */
	UFUNCTION(BlueprintCallable, Category = "MoonToon|Material")
	static FString SetScalarOnInstances(
		const TArray<UMaterialInstanceConstant*>& Instances,
		FName ParameterName,
		float Value);

	/** Static-switch counterpart of SetScalarOnInstances. Recompiles the instances it changes. */
	UFUNCTION(BlueprintCallable, Category = "MoonToon|Material")
	static FString SetStaticSwitchOnInstances(
		const TArray<UMaterialInstanceConstant*>& Instances,
		FName ParameterName,
		bool bValue);

	/** Drops the named overrides (or every override, when the list is empty) back to the parent. */
	UFUNCTION(BlueprintCallable, Category = "MoonToon|Material")
	static FString ResetOverrides(
		const TArray<UMaterialInstanceConstant*>& Instances,
		const TArray<FName>& ParameterNames);

	/**
	 * Fills a preset from what the given instances currently say.
	 *
	 * Only parameters the whole selection agrees on are captured -- a preset holding "whichever value
	 * the first instance happened to have" would be a trap. bOverriddenOnly keeps it to what the
	 * instances actually override, which is almost always what is meant by "save this look".
	 */
	UFUNCTION(BlueprintCallable, Category = "MoonToon|Material")
	static FString CaptureToPreset(
		class UMoonToonMaterialPreset* Preset,
		const TArray<UMaterialInstanceConstant*>& Instances,
		bool bOverriddenOnly = true);

	/**
	 * Writes a preset's values onto every instance as overrides.
	 *
	 * Parameters the target's parent chain does not declare are skipped and named in the report,
	 * rather than written as overrides that resolve to nothing.
	 */
	UFUNCTION(BlueprintCallable, Category = "MoonToon|Material")
	static FString ApplyPreset(
		class UMoonToonMaterialPreset* Preset,
		const TArray<UMaterialInstanceConstant*>& Instances);

	/** Saves the instances' packages. Returns how many were written. */
	UFUNCTION(BlueprintCallable, Category = "MoonToon|Material")
	static int32 SaveInstances(const TArray<UMaterialInstanceConstant*>& Instances);

	/** Reports what each instance overrides, and which of those the parent no longer declares. */
	UFUNCTION(BlueprintCallable, Category = "MoonToon|Material")
	static FString DescribeInstances(const TArray<UMaterialInstanceConstant*>& Instances);
};
