// Copyright Dream Moon. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "MoonToonPerspectiveComponent.generated.h"

class UMeshComponent;

/**
 * Per-character perspective correction ("纸片人" flattening).
 *
 * Add one to a character actor and every mesh component on the actor renders with its view-space
 * depth compressed around the pivot -- close-up faces stop bulging the way a wide lens makes them,
 * while the rest of the scene keeps its normal perspective. The actual work happens engine-side
 * (MoonToonPerspective.ush, injected into the WPO and GBuffer-normal paths of toon-shaded
 * materials); this component only publishes parameters.
 *
 * Transport is custom primitive data, which costs nothing extra: the slots already exist on every
 * primitive in GPUScene, and all-zero defaults mean "off". This component owns float slots 28..35
 * (float4 slots 7 and 8) by project convention -- do not use them for anything else on affected
 * meshes. Materials may read the same slots with CustomPrimitiveData nodes when a per-material
 * twist on the effect is wanted; no extra plumbing needed.
 *
 * CPD contract (keep in sync with MoonToonPerspective.ush):
 *   floats 28..30  pivot position, mesh-component local space
 *   float  31      amount
 *   floats 32..35  fade near, fade far, normal flatten, reserved
 */
UCLASS(ClassGroup = (MoonToon), meta = (BlueprintSpawnableComponent), HideCategories = (Activation, Cooking))
class MOONTOON_API UMoonToonPerspectiveComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UMoonToonPerspectiveComponent();

	/**
	 * How flat. 0 = off, 1 = fully flat (a true billboard). Negative values exaggerate the
	 * perspective instead -- the punch-in look for special shots. Note that negative values expand
	 * the mesh beyond its bounds, so pair them with a Bounds Scale on the mesh component.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Perspective",
		meta = (ClampMin = "-4.0", ClampMax = "1.0", UIMin = "0.0", UIMax = "1.0"))
	float Amount = 0.6f;

	/** Camera-to-pivot distance (cm) at and below which the correction is at full strength. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Perspective", meta = (ClampMin = "0.0"))
	float FadeNearDistance = 100.0f;

	/**
	 * Distance (cm) at which the correction has faded to nothing. Past a few meters the real
	 * perspective is already flat, and keeping the correction just reads as wrong parallax.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Perspective", meta = (ClampMin = "0.0"))
	float FadeFarDistance = 450.0f;

	/**
	 * 0 = lighting keeps the true normals (only the silhouette flattens), 1 = normals follow the
	 * flattening, which softens AO and smooths highlights toward a more 2D look. Artistic choice.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Perspective",
		meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float NormalFlatten = 0.6f;

	/**
	 * Bone or socket whose position anchors the correction: geometry at the pivot's depth keeps its
	 * exact screen position and size. The head, for a portrait-style correction. Falls back to
	 * PivotLocalOffset alone when the target has no such socket (static meshes, props).
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Perspective")
	FName PivotSocketName = TEXT("J_Bip_C_Head");

	/** Added to the socket position; the whole pivot when no socket matches. Mesh-component local space. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Perspective")
	FVector PivotLocalOffset = FVector::ZeroVector;

	//~ UActorComponent
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;
	virtual void OnRegister() override;
	virtual void OnUnregister() override;
	virtual void Deactivate() override;

private:
	/** Writes the CPD block to every mesh component on the owner. Zero amount when disabling. */
	void PushParameters(bool bDisable);
};
