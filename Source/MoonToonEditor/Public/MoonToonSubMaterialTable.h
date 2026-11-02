// Copyright Dream Moon. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "MoonToonSubMaterialTable.generated.h"

class UMaterialInstanceConstant;
class UTexture2D;

/**
 * One sub-material (T22): the parameters it overrides on the merged material instance. Only fields
 * whose override box is ticked are written; every other field keeps the instance's own value.
 *
 * The layout (column, component, override bit) is the one MF_ToonSubMaterial reads and
 * Tools/SubMaterial/submat_common.py writes -- keep the three in step.
 */
USTRUCT(BlueprintType)
struct MOONTOONEDITOR_API FMoonToonSubMaterialRow
{
	GENERATED_BODY()

	/** Display name only (the slot or part this row stands for). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Row")
	FName Name;

	// ---- surface (MF_MoonToonBaseInput) ----

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Surface", meta = (InlineEditConditionToggle))
	bool bOverride_ShadowColor = false;
	/** Shadow Color (rgb). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Surface", meta = (EditCondition = "bOverride_ShadowColor", HideAlphaChannel))
	FLinearColor ShadowColor = FLinearColor(0.5f, 0.5f, 0.5f, 1.0f);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Surface", meta = (InlineEditConditionToggle))
	bool bOverride_Metallic = false;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Surface", meta = (EditCondition = "bOverride_Metallic"))
	float Metallic = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Surface", meta = (InlineEditConditionToggle))
	bool bOverride_Specular = false;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Surface", meta = (EditCondition = "bOverride_Specular"))
	float Specular = 0.5f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Surface", meta = (InlineEditConditionToggle))
	bool bOverride_Roughness = false;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Surface", meta = (EditCondition = "bOverride_Roughness"))
	float Roughness = 0.5f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Surface", meta = (InlineEditConditionToggle))
	bool bOverride_Reflection = false;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Surface", meta = (EditCondition = "bOverride_Reflection"))
	float Reflection = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Surface", meta = (InlineEditConditionToggle))
	bool bOverride_Anisotropy = false;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Surface", meta = (EditCondition = "bOverride_Anisotropy"))
	float Anisotropy = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Surface", meta = (InlineEditConditionToggle))
	bool bOverride_EmissiveScale = false;
	/** Multiplies Emissive Intensity. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Surface", meta = (EditCondition = "bOverride_EmissiveScale"))
	float EmissiveScale = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Surface|Reserved", meta = (InlineEditConditionToggle))
	bool bOverride_RampUVOffset = false;
	/** Reserved: packed, not read by the material yet. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Surface|Reserved", meta = (EditCondition = "bOverride_RampUVOffset"))
	float RampUVOffset = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Surface|Reserved", meta = (InlineEditConditionToggle))
	bool bOverride_BaseColorTint = false;
	/** Reserved: packed (with the strength), not read by the material yet. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Surface|Reserved", meta = (EditCondition = "bOverride_BaseColorTint", HideAlphaChannel))
	FLinearColor BaseColorTint = FLinearColor::White;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Surface|Reserved", meta = (EditCondition = "bOverride_BaseColorTint"))
	float BaseColorTintStrength = 1.0f;

	// ---- feature channels: the generic slots of the feature the material shades with ----

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Feature Channels", meta = (InlineEditConditionToggle))
	bool bOverride_ScalarA = false;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Feature Channels", meta = (EditCondition = "bOverride_ScalarA"))
	float ScalarA = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Feature Channels", meta = (InlineEditConditionToggle))
	bool bOverride_ScalarB = false;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Feature Channels", meta = (EditCondition = "bOverride_ScalarB"))
	float ScalarB = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Feature Channels", meta = (InlineEditConditionToggle))
	bool bOverride_ScalarC = false;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Feature Channels", meta = (EditCondition = "bOverride_ScalarC"))
	float ScalarC = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Feature Channels", meta = (InlineEditConditionToggle))
	bool bOverride_ScalarD = false;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Feature Channels", meta = (EditCondition = "bOverride_ScalarD"))
	float ScalarD = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Feature Channels", meta = (InlineEditConditionToggle))
	bool bOverride_ScalarE = false;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Feature Channels", meta = (EditCondition = "bOverride_ScalarE"))
	float ScalarE = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Feature Channels", meta = (InlineEditConditionToggle))
	bool bOverride_ScalarF = false;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Feature Channels", meta = (EditCondition = "bOverride_ScalarF"))
	float ScalarF = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Feature Channels", meta = (InlineEditConditionToggle))
	bool bOverride_ScalarG = false;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Feature Channels", meta = (EditCondition = "bOverride_ScalarG"))
	float ScalarG = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Feature Channels", meta = (InlineEditConditionToggle))
	bool bOverride_VectorX = false;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Feature Channels", meta = (EditCondition = "bOverride_VectorX"))
	float VectorX = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Feature Channels", meta = (InlineEditConditionToggle))
	bool bOverride_VectorY = false;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Feature Channels", meta = (EditCondition = "bOverride_VectorY"))
	float VectorY = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Feature Channels", meta = (InlineEditConditionToggle))
	bool bOverride_VectorZ = false;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Feature Channels", meta = (EditCondition = "bOverride_VectorZ"))
	float VectorZ = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Feature Channels", meta = (InlineEditConditionToggle))
	bool bOverride_VectorW = false;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Feature Channels", meta = (EditCondition = "bOverride_VectorW"))
	float VectorW = 0.0f;

	// ---- outline (reserved) ----

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Outline|Reserved", meta = (InlineEditConditionToggle))
	bool bOverride_OutlineWidthScale = false;
	/** Reserved: packed, not read by M_MoonToonOutline yet. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Outline|Reserved", meta = (EditCondition = "bOverride_OutlineWidthScale"))
	float OutlineWidthScale = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Outline|Reserved", meta = (InlineEditConditionToggle))
	bool bOverride_OutlineColorScale = false;
	/** Reserved: packed, not read by M_MoonToonOutline yet. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Outline|Reserved", meta = (EditCondition = "bOverride_OutlineColorScale", HideAlphaChannel))
	FLinearColor OutlineColorScale = FLinearColor::White;
};

/**
 * A sub-material table (T22 / G01): up to 16 rows, each the parameter overrides of one part of a merged
 * material. Baked into an 8 x 16 RGBA16F texture T_<table name> next to the asset, which the merged
 * instance references through its "SubMaterial Table" texture parameter (switch "Use SubMaterial
 * Table"); the material picks the row from the sub-material ID (MF_ToonSubMaterialID).
 *
 * Editor-only: the game only needs the baked texture.
 */
UCLASS(BlueprintType)
class MOONTOONEDITOR_API UMoonToonSubMaterialTable : public UDataAsset
{
	GENERATED_BODY()

public:
	static constexpr int32 MaxRows = 16;
	static constexpr int32 NumColumns = 8;

	/** Row i is sub-material ID i. At most 16 rows; extra rows are not baked. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sub-Material Table", meta = (TitleProperty = "Name"))
	TArray<FMoonToonSubMaterialRow> Rows;

	/** Re-bake the texture whenever a row is edited in the details panel. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Bake")
	bool bAutoBake = true;

	/** The baked table (T_<table name> in the same folder; created on the first bake). */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Bake")
	TObjectPtr<UTexture2D> BakedTexture;

	/** The 24-bit override mask of a row (bit layout of submat_common.py FIELDS). */
	UFUNCTION(BlueprintPure, Category = "MoonToon|SubMaterial")
	static int32 GetOverrideMask(const FMoonToonSubMaterialRow& Row);

	/** The table as float texels, row-major, MaxRows x NumColumns (unused rows zero), before the half-float bake. */
	UFUNCTION(BlueprintCallable, Category = "MoonToon|SubMaterial")
	TArray<FLinearColor> PackTexels() const;

	/**
	 * Writes BakedTexture (creating T_<table name> next to the asset when needed) and returns it; the
	 * texture package is left dirty. Null when another kind of asset already holds that name.
	 */
	UFUNCTION(BlueprintCallable, Category = "MoonToon|SubMaterial")
	UTexture2D* BakeTexture();

	/** The details panel button (a CallInEditor function cannot return a value). */
	UFUNCTION(CallInEditor, Category = "Bake")
	void Bake() { BakeTexture(); }

	/** Bakes if needed, then turns on "Use SubMaterial Table" on the instance and points "SubMaterial Table" at the baked texture. */
	UFUNCTION(BlueprintCallable, Category = "MoonToon|SubMaterial")
	bool ApplyToMaterialInstance(UMaterialInstanceConstant* Instance);

	virtual bool IsEditorOnly() const override { return true; }
#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif
};
