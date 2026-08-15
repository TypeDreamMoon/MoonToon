// Copyright Dream Moon. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MoonToonTool.h"
#include "MoonToonMeshInfoTool.generated.h"

/**
 * Read-only inspection of what a mesh actually carries.
 *
 * Every question this answers is one that previously needed either a throwaway Python script or a
 * debug switch in the material: is this mesh baked, which channel holds what, is there a vertex
 * colour stream, and is some other feature already using the channel I am about to write.
 */
UCLASS()
class MOONTOONEDITOR_API UMoonToonMeshInfoTool : public UMoonToonTool
{
	GENERATED_BODY()

public:
	virtual FText GetToolName() const override;
	virtual FText GetToolDescription() const override;
	virtual FText GetRunLabel() const override;
	virtual FName GetToolIconName() const override { return TEXT("Icons.Details"); }
	virtual bool IsDestructive() const override { return false; }
	virtual FString Run(const FMoonToonToolContext& Context) override;

	/** Report every LOD instead of just LOD 0. */
	UPROPERTY(EditAnywhere, Category = "Report")
	bool bAllLODs = false;

	/** Per-channel min/max/mean of the vertex colour stream, and whether each channel is constant. */
	UPROPERTY(EditAnywhere, Category = "Report")
	bool bAnalyzeVertexColors = true;

	/**
	 * Decodes the two bake channel sets and reports what they look like. Costs a second pass over the
	 * wedges; the only reason to turn it off is on very heavy meshes.
	 */
	UPROPERTY(EditAnywhere, Category = "Report")
	bool bDetectBakes = true;

	/** Restrict the vertex-colour and bake statistics to the selected sections. */
	UPROPERTY(EditAnywhere, Category = "Report")
	bool bRespectSectionFilter = true;
};
