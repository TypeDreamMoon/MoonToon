// Copyright Dream Moon. All Rights Reserved.

#include "MoonToonTool.h"

#include "MoonToonMeshTargets.h"

#define LOCTEXT_NAMESPACE "MoonToonTool"

FText UMoonToonTool::GetRunLabel() const
{
	return LOCTEXT("Run", "Run");
}

FString UMoonToonTool::RunOnMeshes(const TArray<UObject*>& Meshes, const TArray<int32>& SectionMaterialIndices)
{
	FMoonToonToolContext Context;
	Context.Meshes.Reserve(Meshes.Num());
	for (UObject* Mesh : Meshes)
	{
		Context.Meshes.Add(Mesh);
	}
	Context.SectionMaterialIndices = SectionMaterialIndices;
	return Run(Context);
}

bool UMoonToonTool::ResolveMeshes(const FMoonToonToolContext& Context, TArray<UObject*>& OutMeshes, FString& OutReport)
{
	OutMeshes.Reset();
	for (const TWeakObjectPtr<UObject>& Weak : Context.Meshes)
	{
		if (UObject* Mesh = Weak.Get())
		{
			if (MoonToonMesh::IsSupportedMesh(Mesh))
			{
				OutMeshes.Add(Mesh);
			}
		}
	}

	if (OutMeshes.Num() == 0)
	{
		OutReport = TEXT("No target mesh. Select a Static Mesh or Skeletal Mesh in the Content Browser, "
			"or an actor using one in the level.");
		return false;
	}
	return true;
}

#undef LOCTEXT_NAMESPACE
