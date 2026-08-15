// Copyright Dream Moon. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/EngineTypes.h"
#include "MoonToonTool.h"
#include "MoonToonCharacterMasterTool.generated.h"

/**
 * Gives a character one material instance that speaks for all of it.
 *
 * A VRM import hands every section an instance parented straight at the shared MoonToon preset, so
 * there is nowhere to say "on this character, the shadow is warmer" -- only somewhere to say it
 * sixty-four times. This inserts the missing level:
 *
 *     M_MoonToon -> MI_Moon_Toon_VRM_TwoSide -> MI_<Character>_Master -> MI_<section>
 *
 * Running it changes nothing about how the character looks. The master is either empty or holds
 * values its children already agreed on, and the children keep every override that was theirs alone.
 */
UCLASS()
class MOONTOONEDITOR_API UMoonToonCharacterMasterTool : public UMoonToonTool
{
	GENERATED_BODY()

public:
	virtual FText GetToolName() const override;
	virtual FText GetToolDescription() const override;
	virtual FText GetRunLabel() const override;
	virtual FName GetToolIconName() const override { return TEXT("Icons.Link"); }
	virtual FString Run(const FMoonToonToolContext& Context) override;

	/** Name for the master, without the MI_ prefix. Empty takes the mesh's, minus its SK_ / SM_. */
	UPROPERTY(EditAnywhere, Category = "Character Master")
	FString CharacterName;

	/** Where the master goes. Empty puts it with the instances it is being inserted above. */
	UPROPERTY(EditAnywhere, Category = "Character Master", meta = (ContentDir))
	FDirectoryPath DestinationFolder;

	/**
	 * Moves overrides that every section already agreed on up to the master.
	 *
	 * Without this the master is an empty pass-through: correct, but it does nothing until somebody
	 * finds the right parameter to put on it. With it, the values that were already character-wide
	 * are on the character.
	 */
	UPROPERTY(EditAnywhere, Category = "Character Master")
	bool bPromoteCommonOverrides = true;

	/**
	 * Include static switches in that promotion.
	 *
	 * Off by default because clearing a switch override has to go through the engine's own entry
	 * point, which republishes and recompiles that instance -- once per switch per section. On a
	 * sixty-section character that is minutes, not seconds.
	 */
	UPROPERTY(EditAnywhere, Category = "Character Master", meta = (EditCondition = "bPromoteCommonOverrides"))
	bool bPromoteStaticSwitches = false;

	/**
	 * Saves the master and everything re-parented under it.
	 *
	 * On by default, and not merely a convenience: a freshly created master that is never written
	 * disappears with the editor, and takes every child's parent with it.
	 */
	UPROPERTY(EditAnywhere, Category = "Character Master")
	bool bSaveWhenDone = true;
};
