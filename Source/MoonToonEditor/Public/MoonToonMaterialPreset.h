// Copyright Dream Moon. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "MoonToonMaterialPreset.generated.h"

class UMaterialInterface;
class UTexture;

/**
 * Values are stored per type rather than as an FMaterialParameterValue so the asset stays readable
 * in the editor and diffable in source control -- a union serialises as whichever member happened to
 * be set, which is exactly the thing that makes a merge conflict unresolvable by hand.
 */
USTRUCT()
struct FMoonToonPresetScalar
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, Category = "Parameter")
	FName Name;

	UPROPERTY(EditAnywhere, Category = "Parameter")
	float Value = 0.0f;
};

USTRUCT()
struct FMoonToonPresetVector
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, Category = "Parameter")
	FName Name;

	UPROPERTY(EditAnywhere, Category = "Parameter")
	FLinearColor Value = FLinearColor::White;
};

USTRUCT()
struct FMoonToonPresetTexture
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, Category = "Parameter")
	FName Name;

	/** Soft: a preset naming forty textures should not drag forty textures into memory to be opened. */
	UPROPERTY(EditAnywhere, Category = "Parameter")
	TSoftObjectPtr<UTexture> Value;
};

USTRUCT()
struct FMoonToonPresetSwitch
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, Category = "Parameter")
	FName Name;

	UPROPERTY(EditAnywhere, Category = "Parameter")
	bool bValue = false;
};

/**
 * A named set of material parameter values that can be dropped onto any instance.
 *
 * The difference from "copy the overrides off that other instance" is that a preset outlives the
 * instance it came from: it can be reviewed, edited by hand, committed, and applied to a character
 * imported next month. Applying one only writes parameters the target's own parent declares, so a
 * face preset landing on a hair instance skips rather than plants dead overrides.
 */
UCLASS(BlueprintType)
class MOONTOONEDITOR_API UMoonToonMaterialPreset : public UDataAsset
{
	GENERATED_BODY()

public:
	/** What this was captured from. Advisory only -- nothing refuses to apply because of it. */
	UPROPERTY(EditAnywhere, Category = "Preset")
	TSoftObjectPtr<UMaterialInterface> CapturedFromParent;

	UPROPERTY(EditAnywhere, Category = "Preset", meta = (MultiLine = true))
	FString Notes;

	UPROPERTY(EditAnywhere, Category = "Values")
	TArray<FMoonToonPresetScalar> Scalars;

	UPROPERTY(EditAnywhere, Category = "Values")
	TArray<FMoonToonPresetVector> Vectors;

	UPROPERTY(EditAnywhere, Category = "Values")
	TArray<FMoonToonPresetTexture> Textures;

	UPROPERTY(EditAnywhere, Category = "Values")
	TArray<FMoonToonPresetSwitch> Switches;

	int32 NumEntries() const { return Scalars.Num() + Vectors.Num() + Textures.Num() + Switches.Num(); }
};
