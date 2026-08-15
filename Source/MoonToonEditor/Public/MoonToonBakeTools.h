// Copyright Dream Moon. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MoonToonTool.h"
#include "MoonToonBakeTools.generated.h"

/**
 * The two UV bakes and the build-settings fixer, as panel tools.
 *
 * These are thin: the implementations live in UMoonToonSmoothNormalTool and are shared with the
 * content-browser right-click entries. What the panel adds is an explicit target, a section filter,
 * per-LOD control, and a report.
 */

/** Bakes the tangent-space smoothed normal, scaled by curvature, into UV2.y / UV3.xy. */
UCLASS()
class MOONTOONEDITOR_API UMoonToonBakeSmoothNormalTool : public UMoonToonTool
{
	GENERATED_BODY()

public:
	virtual FText GetToolName() const override;
	virtual FText GetToolDescription() const override;
	virtual FText GetRunLabel() const override;
	virtual FName GetToolIconName() const override { return TEXT("Icons.Normalize"); }
	// Reimports when the build settings were wrong, and a reimport can change the material list.
	virtual bool InvalidatesSectionList() const override { return true; }
	virtual FString Run(const FMoonToonToolContext& Context) override;

	/**
	 * Recomputing normals or tangents on import throws away exactly what this bake writes, so the
	 * settings are forced first. Turn off only if a reimport would be destructive for other reasons.
	 */
	UPROPERTY(EditAnywhere, Category = "Bake")
	bool bFixBuildSettingsFirst = true;

	/** Bake every LOD. The outline reads the bake at every LOD, so leaving this on is normal. */
	UPROPERTY(EditAnywhere, Category = "Bake")
	bool bAllLODs = true;

	UPROPERTY(EditAnywhere, Category = "Bake", meta = (EditCondition = "!bAllLODs", ClampMin = "0"))
	int32 LODIndex = 0;
};

/** Bakes a fixed world-space direction into tangent space and stores it in UV1.xy / UV2.x. */
UCLASS()
class MOONTOONEDITOR_API UMoonToonBakeFaceForwardTool : public UMoonToonTool
{
	GENERATED_BODY()

public:
	virtual FText GetToolName() const override;
	virtual FText GetToolDescription() const override;
	virtual FText GetRunLabel() const override;
	virtual FName GetToolIconName() const override { return TEXT("Icons.ArrowRight"); }
	// Reimports when the build settings were wrong, and a reimport can change the material list.
	virtual bool InvalidatesSectionList() const override { return true; }
	virtual FString Run(const FMoonToonToolContext& Context) override;

	/**
	 * +Y matches the original EUBP_SmoothNormal's function-local default. Getting this wrong shows up
	 * as facial shadow that runs backwards or sideways relative to the light.
	 */
	UPROPERTY(EditAnywhere, Category = "Bake")
	FVector FaceForwardDirWS = FVector(0.0, 1.0, 0.0);

	UPROPERTY(EditAnywhere, Category = "Bake")
	bool bFixBuildSettingsFirst = true;

	UPROPERTY(EditAnywhere, Category = "Bake")
	bool bAllLODs = true;

	UPROPERTY(EditAnywhere, Category = "Bake", meta = (EditCondition = "!bAllLODs", ClampMin = "0"))
	int32 LODIndex = 0;
};

/** Forces the LOD build settings both bakes depend on, and reimports anything that was wrong. */
UCLASS()
class MOONTOONEDITOR_API UMoonToonFixBuildSettingsTool : public UMoonToonTool
{
	GENERATED_BODY()

public:
	virtual FText GetToolName() const override;
	virtual FText GetToolDescription() const override;
	virtual FText GetRunLabel() const override;
	virtual FName GetToolIconName() const override { return TEXT("Icons.Settings"); }
	// Reimports when the build settings were wrong, and a reimport can change the material list.
	virtual bool InvalidatesSectionList() const override { return true; }
	virtual FString Run(const FMoonToonToolContext& Context) override;
};
