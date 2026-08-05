// Copyright Dream Moon. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "MoonToonStrandPreviewActor.generated.h"

class UMaterialInstanceDynamic;
class UMaterialInterface;
class UMeshComponent;
class UStaticMeshComponent;

/**
 * The strand-tangent live preview: an editor-only, transient actor whose TRANSFORM is the highlight
 * ellipsoid. Move / rotate / scale it with the ordinary viewport gizmo; every editor tick it pushes
 * its transform (plus the scene's light direction) into a dynamic instance of
 * M_MoonToonStrandPreview, which the target's hair sections temporarily wear -- so the Kajiya band
 * the ellipsoid would produce updates live on the posed mesh, no bake involved. The in-engine
 * equivalent of dragging the ellipsoid in MooaToon's Houdini preview.
 *
 * Spawned by UMoonToonStrandTangentTool's Live Preview mode; never saved into the level
 * (RF_Transient). Bake From Ellipsoid commits the current shape through the tool's normal bake
 * (including its first-run channel backup); Exit Preview restores the original materials and
 * removes the actor -- as does simply deleting it.
 */
UCLASS(NotPlaceable)
class MOONTOONEDITOR_API AMoonToonStrandPreviewActor : public AActor
{
	GENERATED_BODY()

public:
	AMoonToonStrandPreviewActor();

	/** Wires the preview to its target and swaps the section materials. Called once by the tool. */
	void InitializePreview(
		class UMoonToonStrandTangentTool* InOwningTool,
		UMeshComponent* InTargetComponent,
		UObject* InTargetMeshAsset,
		const TArray<int32>& InSectionMaterialIndices,
		UMaterialInterface* BandMaterial,
		UMaterialInterface* ShellMaterial);

	virtual void Tick(float DeltaSeconds) override;
	virtual bool ShouldTickIfViewportsOnly() const override { return true; }
	virtual void Destroyed() override;
	virtual void BeginDestroy() override;

	/** Bakes UV1.xy / UV2.x from the CURRENT ellipsoid, backup included, and reports the result. */
	UFUNCTION(CallInEditor, Category = "Strand Preview")
	void BakeFromEllipsoid();

	/** Puts the original materials back and removes this actor. */
	UFUNCTION(CallInEditor, Category = "Strand Preview")
	void ExitPreview();

	/** Follow the level's first directional light. Turn off to aim the band by hand below. */
	UPROPERTY(EditAnywhere, Category = "Strand Preview")
	bool bSyncLightFromScene = true;

	/** Direction TOWARD the light, world space. Only read when the sync above is off. */
	UPROPERTY(EditAnywhere, Category = "Strand Preview", meta = (EditCondition = "!bSyncLightFromScene"))
	FVector LightDirectionOverride = FVector(0.0, 0.0, 1.0);

	/** Kajiya lobe exponent for the preview band. Higher = thinner. Preview-only: the baked result's
	 *  width is tuned on the hair material's own Kajiya parameters afterwards. */
	UPROPERTY(EditAnywhere, Category = "Strand Preview", meta = (ClampMin = "1"))
	float BandExponent = 60.0f;

	/** Bake every LOD when committing. */
	UPROPERTY(EditAnywhere, Category = "Strand Preview")
	bool bBakeAllLODs = true;

	UPROPERTY(EditAnywhere, Category = "Strand Preview", meta = (EditCondition = "!bBakeAllLODs", ClampMin = "0"))
	int32 BakeLODIndex = 0;

private:
	void RestoreMaterials();

	/**
	 * Keeps the actor's transform and the tool's ellipsoid properties equal, in both directions:
	 * drag the gizmo and the panel's numbers follow (and Fit switches off, because the shape is now
	 * explicit); type into the panel and the actor moves. Whichever side changed last wins, so a
	 * panel-side Bake always commits the ellipsoid that is actually on screen.
	 */
	void SyncEllipsoidWithTool();

	UPROPERTY(Transient)
	TWeakObjectPtr<class UMoonToonStrandTangentTool> OwningTool;

	// Last values both sides agreed on, in the target mesh's local space.
	FVector LastSyncedCenter = FVector::ZeroVector;
	FVector LastSyncedRadii = FVector::ZeroVector;
	FRotator LastSyncedRotation = FRotator::ZeroRotator;
	bool bHasSyncedOnce = false;

	/** The ellipsoid shell the gizmo grabs. Unit engine sphere (radius 50): actor scale * 50 = radii. */
	UPROPERTY()
	TObjectPtr<UStaticMeshComponent> ShellComponent;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> PreviewMID;

	UPROPERTY(Transient)
	TWeakObjectPtr<UMeshComponent> TargetComponent;

	UPROPERTY(Transient)
	TWeakObjectPtr<UObject> TargetMeshAsset;

	UPROPERTY(Transient)
	TArray<int32> SectionMaterialIndices;

	/** Slot index -> material that was there before the preview took over. */
	UPROPERTY(Transient)
	TMap<int32, TObjectPtr<UMaterialInterface>> OriginalMaterials;

	bool bMaterialsRestored = false;
};
