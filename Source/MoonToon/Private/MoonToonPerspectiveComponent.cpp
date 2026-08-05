// Copyright Dream Moon. All Rights Reserved.

#include "MoonToonPerspectiveComponent.h"

#include "Components/MeshComponent.h"
#include "Components/SkinnedMeshComponent.h"
#include "GameFramework/Actor.h"

namespace
{
	// Float indices of the two CPD float4 slots. Keep in sync with MoonToonPerspective.ush
	// (MOONTOON_PERSPECTIVE_CPD_PIVOT / _TUNING are the same values divided by four).
	constexpr int32 PivotAmountFloatIndex = 28;
	constexpr int32 TuningFloatIndex = 32;
}

UMoonToonPerspectiveComponent::UMoonToonPerspectiveComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	// After animation, so a bone pivot carries this frame's pose rather than last frame's.
	PrimaryComponentTick.TickGroup = TG_PostUpdateWork;
	// The effect should be tunable in the editor viewport without entering PIE.
	bTickInEditor = true;
	bAutoActivate = true;
}

void UMoonToonPerspectiveComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
	PushParameters(/*bDisable=*/false);
}

void UMoonToonPerspectiveComponent::OnRegister()
{
	Super::OnRegister();
	PushParameters(/*bDisable=*/!IsActive());
}

void UMoonToonPerspectiveComponent::OnUnregister()
{
	// CPD outlives its writer; without this, removing the component would leave the character
	// permanently flattened at the last written amount.
	PushParameters(/*bDisable=*/true);
	Super::OnUnregister();
}

void UMoonToonPerspectiveComponent::Deactivate()
{
	Super::Deactivate();
	// Deactivation stops the tick, so the zero has to be pushed here, once.
	PushParameters(/*bDisable=*/true);
}

void UMoonToonPerspectiveComponent::PushParameters(bool bDisable)
{
	const AActor* Owner = GetOwner();
	if (!Owner)
	{
		return;
	}

	const float EffectiveAmount = bDisable ? 0.0f : Amount;

	TInlineComponentArray<UMeshComponent*> MeshComponents(Owner);
	for (UMeshComponent* Mesh : MeshComponents)
	{
		if (!Mesh || !Mesh->IsRegistered())
		{
			continue;
		}

		// Pivot in the mesh component's own space: a bone/socket when one matches (skinned
		// characters), the plain local offset otherwise (props, static meshes).
		FVector PivotLocal = PivotLocalOffset;
		if (!PivotSocketName.IsNone() && Mesh->DoesSocketExist(PivotSocketName))
		{
			if (const USkinnedMeshComponent* Skinned = Cast<USkinnedMeshComponent>(Mesh))
			{
				PivotLocal = Skinned->GetSocketTransform(PivotSocketName, RTS_Component).GetLocation() + PivotLocalOffset;
			}
		}

		Mesh->SetCustomPrimitiveDataVector4(PivotAmountFloatIndex,
			FVector4(PivotLocal.X, PivotLocal.Y, PivotLocal.Z, EffectiveAmount));
		Mesh->SetCustomPrimitiveDataVector4(TuningFloatIndex,
			FVector4(FadeNearDistance, FadeFarDistance, NormalFlatten, 0.0f));
	}
}
