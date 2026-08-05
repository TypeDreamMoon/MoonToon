// Copyright Dream Moon. All Rights Reserved.

#include "MoonToonStrandPreviewActor.h"

#include "Components/MeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/DirectionalLight.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "Misc/MessageDialog.h"
#include "MoonToonStrandTangentTool.h"
#include "UObject/ConstructorHelpers.h"

namespace
{
	// The engine's basic sphere is 100cm across, so actor scale * this = ellipsoid radii.
	constexpr float GShellSphereRadius = 50.0f;
}

AMoonToonStrandPreviewActor::AMoonToonStrandPreviewActor()
{
	PrimaryActorTick.bCanEverTick = true;
	bIsEditorOnlyActor = true;

	ShellComponent = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Shell"));
	SetRootComponent(ShellComponent);
	ShellComponent->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	ShellComponent->SetCastShadow(false);

	// The plugin's own copy of the basic sphere, deliberately NOT /Engine/BasicShapes/Sphere: the
	// panel used to retarget onto whatever the selected actor renders, and with the engine sphere
	// there that pointed every tool (including the bake) at shared engine content.
	static ConstructorHelpers::FObjectFinder<UStaticMesh> SphereFinder(
		TEXT("/MoonToon/Assets/Debug/SM_MoonToonPreviewSphere.SM_MoonToonPreviewSphere"));
	if (SphereFinder.Succeeded())
	{
		ShellComponent->SetStaticMesh(SphereFinder.Object);
	}
}

void AMoonToonStrandPreviewActor::InitializePreview(
	UMoonToonStrandTangentTool* InOwningTool,
	UMeshComponent* InTargetComponent,
	UObject* InTargetMeshAsset,
	const TArray<int32>& InSectionMaterialIndices,
	UMaterialInterface* BandMaterial,
	UMaterialInterface* ShellMaterial)
{
	OwningTool = InOwningTool;
	TargetComponent = InTargetComponent;
	TargetMeshAsset = InTargetMeshAsset;
	SectionMaterialIndices = InSectionMaterialIndices;
	SetActorLabel(TEXT("MoonToonStrandPreview"), /*bMarkDirty=*/false);

	if (ShellMaterial && ShellComponent)
	{
		ShellComponent->SetMaterial(0, ShellMaterial);
	}

	if (!InTargetComponent || !BandMaterial)
	{
		return;
	}

	PreviewMID = UMaterialInstanceDynamic::Create(BandMaterial, this);

	// Swap only the selected sections; everything else keeps rendering normally so the band is
	// judged against the real face and clothes.
	const int32 NumSlots = InTargetComponent->GetNumMaterials();
	for (int32 SlotIndex = 0; SlotIndex < NumSlots; ++SlotIndex)
	{
		const bool bIncluded =
			SectionMaterialIndices.Num() == 0 || SectionMaterialIndices.Contains(SlotIndex);
		if (!bIncluded)
		{
			continue;
		}
		OriginalMaterials.Add(SlotIndex, InTargetComponent->GetMaterial(SlotIndex));
		InTargetComponent->SetMaterial(SlotIndex, PreviewMID);
	}
}

void AMoonToonStrandPreviewActor::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	SyncEllipsoidWithTool();

	if (!PreviewMID)
	{
		return;
	}

	const FQuat Rotation = GetActorQuat();
	const FVector Radii = GetActorScale3D().GetAbs() * GShellSphereRadius;

	PreviewMID->SetVectorParameterValue(TEXT("Ellipsoid_Center"), GetActorLocation());
	PreviewMID->SetVectorParameterValue(TEXT("Ellipsoid_Radii"), Radii);
	PreviewMID->SetVectorParameterValue(TEXT("Ellipsoid_Basis_X"), Rotation.GetAxisX());
	PreviewMID->SetVectorParameterValue(TEXT("Ellipsoid_Basis_Y"), Rotation.GetAxisY());
	PreviewMID->SetVectorParameterValue(TEXT("Ellipsoid_Basis_Z"), Rotation.GetAxisZ());
	PreviewMID->SetScalarParameterValue(TEXT("Band_Exponent"), BandExponent);

	FVector LightDirection = LightDirectionOverride.GetSafeNormal();
	if (bSyncLightFromScene)
	{
		if (UWorld* World = GetWorld())
		{
			for (TActorIterator<ADirectionalLight> It(World); It; ++It)
			{
				// Forward points AWAY from the light along its rays; the shader wants TOWARD it.
				LightDirection = -It->GetActorForwardVector();
				break;
			}
		}
	}
	if (LightDirection.IsNearlyZero())
	{
		LightDirection = FVector::UpVector;
	}
	PreviewMID->SetVectorParameterValue(TEXT("Light_Direction"), LightDirection);
}

void AMoonToonStrandPreviewActor::SyncEllipsoidWithTool()
{
	UMoonToonStrandTangentTool* Tool = OwningTool.Get();
	UMeshComponent* Component = TargetComponent.Get();
	if (!Tool || !Component)
	{
		return;
	}

	const FTransform ComponentToWorld = Component->GetComponentTransform();
	const FTransform LocalEllipsoid = GetActorTransform().GetRelativeTransform(ComponentToWorld);
	const FVector ActorCenter = LocalEllipsoid.GetLocation();
	const FVector ActorRadii = LocalEllipsoid.GetScale3D().GetAbs() * GShellSphereRadius;
	const FRotator ActorRotation = LocalEllipsoid.Rotator();

	const bool bActorMoved = !bHasSyncedOnce
		|| !ActorCenter.Equals(LastSyncedCenter, 0.01)
		|| !ActorRadii.Equals(LastSyncedRadii, 0.01)
		|| !ActorRotation.Equals(LastSyncedRotation, 0.01);

	if (bActorMoved)
	{
		// Actor -> panel. Fit goes off: the shape is now whatever was dragged, not a bounds fit,
		// and the offset folds into the absolute centre so the two cannot double up.
		Tool->bFitEllipsoidToSelection = false;
		Tool->EllipsoidCenter = ActorCenter;
		Tool->EllipsoidCenterOffset = FVector::ZeroVector;
		Tool->EllipsoidRadii = ActorRadii;
		Tool->EllipsoidRotation = ActorRotation;
		++Tool->ExternalEditSerial;

		LastSyncedCenter = ActorCenter;
		LastSyncedRadii = ActorRadii;
		LastSyncedRotation = ActorRotation;
		bHasSyncedOnce = true;
		return;
	}

	// Panel -> actor. Only meaningful once the shape is explicit; re-ticking Fit hands control back
	// to the bounds fit, which this actor cannot evaluate (it has no mesh data), so it is ignored.
	if (Tool->bFitEllipsoidToSelection)
	{
		return;
	}

	const FVector ToolCenter = Tool->EllipsoidCenter + Tool->EllipsoidCenterOffset;
	const FVector ToolRadii = Tool->EllipsoidRadii.ComponentMax(FVector(0.1));
	const FRotator ToolRotation = Tool->EllipsoidRotation;

	const bool bPanelChanged =
		!ToolCenter.Equals(LastSyncedCenter, 0.01)
		|| !ToolRadii.Equals(LastSyncedRadii, 0.01)
		|| !ToolRotation.Equals(LastSyncedRotation, 0.01);

	if (bPanelChanged)
	{
		const FTransform NewLocal(ToolRotation.Quaternion(), ToolCenter, ToolRadii / GShellSphereRadius);
		SetActorTransform(NewLocal * ComponentToWorld);

		LastSyncedCenter = ToolCenter;
		LastSyncedRadii = ToolRadii;
		LastSyncedRotation = ToolRotation;
	}
}

void AMoonToonStrandPreviewActor::BakeFromEllipsoid()
{
	UObject* Mesh = TargetMeshAsset.Get();
	UMeshComponent* Component = TargetComponent.Get();
	if (!Mesh || !Component)
	{
		FMessageDialog::Open(EAppMsgType::Ok, NSLOCTEXT("MoonToonStrandPreview", "BakeNoTarget",
			"The preview lost its target mesh -- exit and start the live preview again."));
		return;
	}

	// The bake works in the mesh's local space; fold the component transform out of the actor's.
	const FTransform LocalEllipsoid = GetActorTransform().GetRelativeTransform(Component->GetComponentTransform());
	const FVector LocalCenter = LocalEllipsoid.GetLocation();
	const FVector LocalRadii = LocalEllipsoid.GetScale3D().GetAbs() * GShellSphereRadius;
	const FQuat LocalRotation = LocalEllipsoid.GetRotation();

	const FString Report = UMoonToonStrandTangentTool::BakeEllipsoidChannels(
		Mesh, SectionMaterialIndices, LocalCenter, LocalRadii, LocalRotation, bBakeAllLODs, BakeLODIndex);

	FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(Report));
}

void AMoonToonStrandPreviewActor::ExitPreview()
{
	Destroy();
}

void AMoonToonStrandPreviewActor::Destroyed()
{
	RestoreMaterials();
	Super::Destroyed();
}

void AMoonToonStrandPreviewActor::BeginDestroy()
{
	// Destroyed() covers Delete and Exit Preview, but an actor can also go away without it --
	// level change, undo of the spawn, GC of an orphan. Leaving the preview MID on the hair would
	// be invisible until someone wondered why the character renders grey, so restore here too.
	RestoreMaterials();
	Super::BeginDestroy();
}

void AMoonToonStrandPreviewActor::RestoreMaterials()
{
	if (bMaterialsRestored)
	{
		return;
	}
	bMaterialsRestored = true;

	if (UMeshComponent* Component = TargetComponent.Get())
	{
		for (const TPair<int32, TObjectPtr<UMaterialInterface>>& Original : OriginalMaterials)
		{
			Component->SetMaterial(Original.Key, Original.Value);
		}
	}
	OriginalMaterials.Empty();
}
