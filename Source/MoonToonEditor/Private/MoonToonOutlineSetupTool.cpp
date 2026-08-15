// Copyright Dream Moon. All Rights Reserved.

#include "MoonToonOutlineSetupTool.h"

#include "AssetToolsModule.h"
#include "Components/MeshComponent.h"
#include "Components/SkinnedMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Factories/MaterialInstanceConstantFactoryNew.h"
#include "IAssetTools.h"
#include "MaterialEditingLibrary.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialInterface.h"
#include "Misc/PackageName.h"
#include "MoonToonEditorBPLibrary.h"
#include "MoonToonMeshTargets.h"
#include "UObject/Package.h"
#include "UObject/UObjectIterator.h"

#define LOCTEXT_NAMESPACE "MoonToonOutlineSetupTool"

namespace
{
	/**
	 * The parameters that decide the opacity mask, and nothing else.
	 *
	 * Copying the whole parameter set would work -- M_MoonToonOutline pulls in the same
	 * MF_MoonToonBaseInput as M_MoonToon, so every name resolves -- and would be a mistake: the
	 * static switches in there are shader permutations, and a hull that is never lit like the
	 * surface has no use for the shading ones. These are the ones that reach OpacityMask.
	 */
	const FName TextureParams[] = {
		TEXT("Base Color Map"),
		TEXT("Opacity Map"),
	};

	const FName ScalarParams[] = {
		TEXT("Opacity"),
		TEXT("Global UV Channel"),
	};

	const FName VectorParams[] = {
		// A ChannelMaskParameter is a vector parameter as far as the instance is concerned.
		TEXT("Opacity Channel"),
		TEXT("Global UV Scale/Offset"),
	};

	const FName SwitchParams[] = {
		TEXT("Opacity From Base Color Map"),
		TEXT("Opacity From Vertex Color"),
		TEXT("Use Vertex Color In sRGB Space"),
	};

	const TCHAR* KindName(EMoonToonOutlineSectionKind Kind)
	{
		switch (Kind)
		{
		case EMoonToonOutlineSectionKind::Opaque:                    return TEXT("opaque");
		case EMoonToonOutlineSectionKind::Masked:                    return TEXT("masked");
		case EMoonToonOutlineSectionKind::Translucent:               return TEXT("translucent");
		case EMoonToonOutlineSectionKind::DuplicateOfAnotherSection: return TEXT("DUPLICATE");
		}
		return TEXT("?");
	}

	/** Object-path folder of an asset, e.g. /Game/Chars/Xilian for /Game/Chars/Xilian/SK_Foo. */
	FString GetAssetFolder(const UObject* Asset)
	{
		return FPackageName::GetLongPackagePath(Asset->GetOutermost()->GetName());
	}

	/** Strips the characters a package name cannot carry, so a slot name can become an asset name. */
	FString SanitizeForAssetName(const FString& In)
	{
		FString Out = In;
		for (const TCHAR Bad : {TEXT('/'), TEXT('\\'), TEXT(':'), TEXT('*'), TEXT('?'), TEXT('"'),
								TEXT('<'), TEXT('>'), TEXT('|'), TEXT(' '), TEXT('.'), TEXT(',')})
		{
			Out.ReplaceCharInline(Bad, TEXT('_'));
		}
		// '+' is legal in a package name but reads badly in one, and on an MMD import it is on most
		// duplicate slots.
		Out.ReplaceCharInline(TEXT('+'), TEXT('P'));
		return Out;
	}

	/**
	 * A triangle's position, order-independent.
	 *
	 * Order-independent because a duplicate need not repeat the same winding -- and on the mesh this
	 * was measured against it does repeat it, but relying on that would make the test fail silently
	 * on the first import that does not. Snapping to a grid first so that two copies written by
	 * different exporter passes still land on the same key.
	 */
	struct FTriangleKey
	{
		int64 A = 0, B = 0, C = 0;

		bool operator==(const FTriangleKey& Other) const
		{
			return A == Other.A && B == Other.B && C == Other.C;
		}
	};

	uint32 GetTypeHash(const FTriangleKey& Key)
	{
		return HashCombine(HashCombine(::GetTypeHash(Key.A), ::GetTypeHash(Key.B)), ::GetTypeHash(Key.C));
	}

	/**
	 * Rebuilds the render state of every component using this mesh.
	 *
	 * Writing the array to the asset is invisible without this. A scene proxy copies the per-slot
	 * outline materials once, at construction, and nothing about editing the asset tells the
	 * already-built proxies to start over -- so the setup looked like it had done nothing until the
	 * level was reloaded. Returns how many components were refreshed, which is worth reporting: zero
	 * means the change will only show up the next time the mesh is loaded.
	 */
	int32 RefreshComponentsUsing(const UObject* Mesh)
	{
		int32 NumRefreshed = 0;
		for (TObjectIterator<UMeshComponent> It; It; ++It)
		{
			UMeshComponent* Component = *It;
			if (!IsValid(Component))
			{
				continue;
			}

			const UObject* ComponentMesh = nullptr;
			if (const USkinnedMeshComponent* Skinned = Cast<USkinnedMeshComponent>(Component))
			{
				ComponentMesh = Skinned->GetSkinnedAsset();
			}
			else if (const UStaticMeshComponent* Static = Cast<UStaticMeshComponent>(Component))
			{
				ComponentMesh = Static->GetStaticMesh();
			}

			if (ComponentMesh == Mesh)
			{
				Component->MarkRenderStateDirty();
				++NumRefreshed;
			}
		}
		return NumRefreshed;
	}

	int64 QuantizePoint(const FVector3f& P, float Tolerance)
	{
		const double Inv = 1.0 / FMath::Max(Tolerance, UE_KINDA_SMALL_NUMBER);
		// Three coordinates folded into one 64-bit value via a cheap mix. A collision only costs a
		// false coincidence between two triangles, which the per-section fraction then dilutes.
		const int64 X = static_cast<int64>(FMath::RoundToDouble(P.X * Inv));
		const int64 Y = static_cast<int64>(FMath::RoundToDouble(P.Y * Inv));
		const int64 Z = static_cast<int64>(FMath::RoundToDouble(P.Z * Inv));
		return X * 73856093LL ^ Y * 19349663LL ^ Z * 83492791LL;
	}
}

UMoonToonOutlineSetupTool::UMoonToonOutlineSetupTool()
{
	OutlineMaterialTemplate = LoadObject<UMaterialInterface>(
		nullptr, TEXT("/MoonToon/MaterialInstance/MI_MoonToonOutline.MI_MoonToonOutline"));
}

FText UMoonToonOutlineSetupTool::GetToolName() const
{
	return LOCTEXT("OutlineSetupName", "Outline Setup");
}

FText UMoonToonOutlineSetupTool::GetToolDescription() const
{
	return LOCTEXT("OutlineSetupDesc",
		"逐 section 配置反向挤出描边, 而不是整根 mesh 一刀切.\n\n"
		"描边 pass 会把每个 section 复制一份、翻转 culling 再画一遍. 在导入角色上这会同时踩两个坑: "
		"位置完全重合的重复 section(MMD/PMX 常见, 多为 '+' 后缀)各自生成一份 hull 互相打架; "
		"不透明 hull 把纱、蕾丝、发片的 alpha 挖洞全部填实.\n\n"
		"工具按几何重合找出重复 section(每对只留一个画描边), 按 blend mode 找出半透明的(不画), "
		"其余按各自的 alpha 生成描边材质实例, 结果写进 MoonOutlineMaterialSlots.\n\n"
		"注意: 绕序和外扩偏移都不是有效判据 —— 实测重复面绕序相同、位置精确重合. 详见头文件注释.");
}

FText UMoonToonOutlineSetupTool::GetRunLabel() const
{
	return bPreviewOnly
		? LOCTEXT("OutlineSetupPreview", "Classify")
		: LOCTEXT("OutlineSetupRun", "Set Up Outlines");
}

bool UMoonToonOutlineSetupTool::BuildPlan(UObject* Mesh, TArray<FSlotPlan>& OutPlan, TArray<FString>& Lines) const
{
	FMoonToonLODFaces Faces;
	if (!MoonToonMesh::GetFaces(Mesh, ClassifyFromLOD, Faces) || !Faces.IsValid())
	{
		Lines.Add(FString::Printf(TEXT("  LOD %d has no import data; nothing to classify."), ClassifyFromLOD));
		return false;
	}

	TArray<FMoonToonSectionInfo> Sections;
	if (!MoonToonMesh::GetSections(Mesh, Faces, Sections))
	{
		Lines.Add(TEXT("  Could not read the section list."));
		return false;
	}

	TArray<FVector3f> Positions, Normals, Tangents, Binormals;
	TArray<int32> VertexIndices;
	TArray<FColor> Colors;
	TArray<FVector2f> UV0s, UV1s, UV2s, UV3s;
	UMoonToonEditorBPLibrary::MoonGetMeshData(Mesh, ClassifyFromLOD, Positions, VertexIndices, Normals,
		Tangents, Binormals, Colors, UV0s, UV1s, UV2s, UV3s);

	// --- Duplicate detection ------------------------------------------------------------------
	//
	// Framed as coverage, not as pairing. Pairwise "A is a duplicate of B" cannot decide a group:
	// on this mesh slots 19, 44 and 45 are all the same surface, each naming 45 as its strongest
	// partner, so excluding 45 leaves 19 and 44 both drawing the identical hull. What the outline
	// pass actually needs is that every surface be hulled once -- so sections claim surface in turn,
	// biggest first, and a section is a duplicate when the surface it would hull is already taken.
	TMap<int32, int32> DuplicateCoveredBy; // material index -> the material that already covers it
	TMap<int32, float> DuplicateCoveredFraction;

	if (bExcludeDuplicateSections)
	{
		TArray<FTriangleKey> KeyByFace;
		KeyByFace.SetNum(Faces.Wedges.Num());
		TMap<int32, TArray<int32>> FacesByMaterial;

		for (int32 FaceIndex = 0; FaceIndex < Faces.Wedges.Num(); ++FaceIndex)
		{
			const FIntVector& W = Faces.Wedges[FaceIndex];
			if (!VertexIndices.IsValidIndex(W.X) || !VertexIndices.IsValidIndex(W.Y) || !VertexIndices.IsValidIndex(W.Z))
			{
				continue;
			}
			const int32 I0 = VertexIndices[W.X];
			const int32 I1 = VertexIndices[W.Y];
			const int32 I2 = VertexIndices[W.Z];
			if (!Positions.IsValidIndex(I0) || !Positions.IsValidIndex(I1) || !Positions.IsValidIndex(I2))
			{
				continue;
			}

			int64 Q[3] = {
				QuantizePoint(Positions[I0], DuplicateWeldTolerance),
				QuantizePoint(Positions[I1], DuplicateWeldTolerance),
				QuantizePoint(Positions[I2], DuplicateWeldTolerance),
			};
			// Sorted, so the key does not depend on which corner the exporter started from or on
			// which way round the triangle is wound.
			if (Q[0] > Q[1]) { Swap(Q[0], Q[1]); }
			if (Q[1] > Q[2]) { Swap(Q[1], Q[2]); }
			if (Q[0] > Q[1]) { Swap(Q[0], Q[1]); }

			KeyByFace[FaceIndex] = FTriangleKey{Q[0], Q[1], Q[2]};
			if (Faces.MaterialIndices.IsValidIndex(FaceIndex))
			{
				FacesByMaterial.FindOrAdd(Faces.MaterialIndices[FaceIndex]).Add(FaceIndex);
			}
		}

		// Biggest section first, so the one that covers the most surface is the one that keeps it.
		// The index tie-break only exists to make the result stable across runs -- two sections with
		// the same geometry produce the same hull either way, but an unstable pick would churn the
		// generated assets on every re-run.
		TArray<int32> Order;
		FacesByMaterial.GetKeys(Order);
		Order.Sort([&FacesByMaterial](int32 A, int32 B)
		{
			const int32 CountA = FacesByMaterial[A].Num();
			const int32 CountB = FacesByMaterial[B].Num();
			return CountA != CountB ? CountA > CountB : A < B;
		});

		TMap<FTriangleKey, int32> CoveredBy; // surface -> the material that claimed it
		for (const int32 MaterialIndex : Order)
		{
			const TArray<int32>& SectionFaces = FacesByMaterial[MaterialIndex];
			if (SectionFaces.Num() == 0)
			{
				continue;
			}

			int32 NumAlreadyCovered = 0;
			TMap<int32, int32> CoveredByWhom;
			for (const int32 FaceIndex : SectionFaces)
			{
				if (const int32* Owner = CoveredBy.Find(KeyByFace[FaceIndex]))
				{
					++NumAlreadyCovered;
					CoveredByWhom.FindOrAdd(*Owner)++;
				}
			}

			const float CoveredFraction = static_cast<float>(NumAlreadyCovered) / SectionFaces.Num();
			if (CoveredFraction >= DuplicateFaceFraction)
			{
				// Name the section that covers the most of it, which is the useful one to report.
				int32 BestOwner = INDEX_NONE;
				int32 BestCount = 0;
				for (const TPair<int32, int32>& Entry : CoveredByWhom)
				{
					if (Entry.Value > BestCount)
					{
						BestCount = Entry.Value;
						BestOwner = Entry.Key;
					}
				}
				DuplicateCoveredBy.Add(MaterialIndex, BestOwner);
				DuplicateCoveredFraction.Add(MaterialIndex, CoveredFraction);
				continue;
			}

			// Draws, and therefore claims every surface it touches -- including the part that was
			// already covered, which costs nothing and keeps the map simple.
			for (const int32 FaceIndex : SectionFaces)
			{
				CoveredBy.Add(KeyByFace[FaceIndex], MaterialIndex);
			}
		}
	}

	// --- Classification -----------------------------------------------------------------------
	OutPlan.Reset(Sections.Num());
	for (const FMoonToonSectionInfo& Section : Sections)
	{
		FSlotPlan Plan;
		Plan.MaterialIndex = Section.MaterialIndex;
		Plan.SlotName = Section.SlotName;
		Plan.SourceMaterial = Section.Material.Get();
		Plan.NumFaces = Section.NumTriangles;

		if (const int32* CoveredBy = DuplicateCoveredBy.Find(Section.MaterialIndex))
		{
			Plan.DuplicateOfMaterialIndex = *CoveredBy;
			Plan.DuplicateFraction = DuplicateCoveredFraction.FindRef(Section.MaterialIndex);
		}

		if (Plan.SourceMaterial == nullptr)
		{
			Plan.Kind = EMoonToonOutlineSectionKind::Opaque;
		}
		else
		{
			// Tested against the two modes that have a hull worth drawing rather than against a list
			// of blended ones: the blend-mode enum grows, and a mode this tool has never heard of
			// should default to "leave it alone", not to "wrap it in an opaque shell".
			switch (Plan.SourceMaterial->GetBlendMode())
			{
			case BLEND_Opaque:
				Plan.Kind = EMoonToonOutlineSectionKind::Opaque;
				break;
			case BLEND_Masked:
				Plan.Kind = EMoonToonOutlineSectionKind::Masked;
				break;
			default:
				Plan.Kind = EMoonToonOutlineSectionKind::Translucent;
				break;
			}
		}

		// Being a duplicate overrides the blend-mode kind: the surface is already hulled, so what
		// this section's own material would have contributed no longer matters.
		if (Plan.DuplicateOfMaterialIndex != INDEX_NONE)
		{
			Plan.Kind = EMoonToonOutlineSectionKind::DuplicateOfAnotherSection;
		}

		OutPlan.Add(Plan);
	}

	return true;
}

UMaterialInterface* UMoonToonOutlineSetupTool::GenerateSlotOutlineMaterial(
	UObject* Mesh, const FSlotPlan& Plan, TArray<FString>& Lines) const
{
	if (OutlineMaterialTemplate == nullptr)
	{
		return nullptr;
	}

	// An opaque section needs nothing of its own: one shared instance is one fewer asset, one fewer
	// shader map, and one fewer thing to keep in sync when the width is retuned.
	if (Plan.Kind == EMoonToonOutlineSectionKind::Opaque || !bCopySectionOpacity || Plan.SourceMaterial == nullptr)
	{
		return OutlineMaterialTemplate;
	}

	const FString Folder = OutputFolder.IsEmpty()
		? GetAssetFolder(Mesh) / TEXT("MoonOutline")
		: OutputFolder;
	const FString AssetName = FString::Printf(TEXT("MI_Outline_%s_%s"),
		*Mesh->GetName(), *SanitizeForAssetName(Plan.SlotName.ToString()));
	const FString PackageName = Folder / AssetName;

	UMaterialInstanceConstant* Instance = LoadObject<UMaterialInstanceConstant>(nullptr, *(PackageName + TEXT(".") + AssetName));
	if (Instance == nullptr)
	{
		IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();
		UMaterialInstanceConstantFactoryNew* Factory = NewObject<UMaterialInstanceConstantFactoryNew>();
		Factory->InitialParent = OutlineMaterialTemplate;
		Instance = Cast<UMaterialInstanceConstant>(
			AssetTools.CreateAsset(AssetName, Folder, UMaterialInstanceConstant::StaticClass(), Factory));
		if (Instance == nullptr)
		{
			Lines.Add(FString::Printf(TEXT("    ! could not create %s"), *PackageName));
			return OutlineMaterialTemplate;
		}
	}
	else if (Instance->Parent != OutlineMaterialTemplate)
	{
		// Re-running after the template was changed should retarget, not leave a stale parent behind.
		Instance->SetParentEditorOnly(OutlineMaterialTemplate);
	}

	// Masked, and cut by the section's own alpha. Without the override the hull is opaque and fills
	// in every hole the section's cutout was there to make.
	Instance->BasePropertyOverrides.bOverride_BlendMode = true;
	Instance->BasePropertyOverrides.BlendMode = BLEND_Masked;

	const UMaterialInterface* Source = Plan.SourceMaterial;
	for (const FName& ParamName : TextureParams)
	{
		UTexture* Value = nullptr;
		if (Source->GetTextureParameterValue(FHashedMaterialParameterInfo(ParamName), Value) && Value)
		{
			UMaterialEditingLibrary::SetMaterialInstanceTextureParameterValue(Instance, ParamName, Value);
		}
	}
	for (const FName& ParamName : ScalarParams)
	{
		float Value = 0.0f;
		if (Source->GetScalarParameterValue(FHashedMaterialParameterInfo(ParamName), Value))
		{
			UMaterialEditingLibrary::SetMaterialInstanceScalarParameterValue(Instance, ParamName, Value);
		}
	}
	for (const FName& ParamName : VectorParams)
	{
		FLinearColor Value = FLinearColor::White;
		if (Source->GetVectorParameterValue(FHashedMaterialParameterInfo(ParamName), Value))
		{
			UMaterialEditingLibrary::SetMaterialInstanceVectorParameterValue(Instance, ParamName, Value);
		}
	}
	for (const FName& ParamName : SwitchParams)
	{
		bool Value = false;
		FGuid Guid;
		if (Source->GetStaticSwitchParameterValue(FHashedMaterialParameterInfo(ParamName), Value, Guid))
		{
			UMaterialEditingLibrary::SetMaterialInstanceStaticSwitchParameterValue(Instance, ParamName, Value);
		}
	}

	// The clip value is a base property, not a parameter, so it has to be carried across by hand
	// -- and it matters: a hull cut at a different threshold than the surface leaves a fringe.
	Instance->BasePropertyOverrides.bOverride_OpacityMaskClipValue = true;
	Instance->BasePropertyOverrides.OpacityMaskClipValue = Source->GetOpacityMaskClipValue();

	UMaterialEditingLibrary::UpdateMaterialInstance(Instance);
	Instance->MarkPackageDirty();
	return Instance;
}

void UMoonToonOutlineSetupTool::ApplyPlan(UObject* Mesh, const TArray<FSlotPlan>& Plan, TArray<FString>& Lines) const
{
	// Sized to the highest material index present, not to the plan's length: the array is indexed by
	// material slot, and a mesh whose sections skip an index would otherwise shift every entry after
	// the gap onto the wrong slot.
	int32 NumSlots = 0;
	for (const FSlotPlan& Row : Plan)
	{
		NumSlots = FMath::Max(NumSlots, Row.MaterialIndex + 1);
	}

	TArray<UMaterialInterface*> Slots;
	Slots.SetNumZeroed(NumSlots);
	for (const FSlotPlan& Row : Plan)
	{
		if (Slots.IsValidIndex(Row.MaterialIndex))
		{
			Slots[Row.MaterialIndex] = Row.OutlineMaterial;
		}
	}

	if (bWriteToMeshAsset)
	{
		if (USkeletalMesh* SkeletalMesh = Cast<USkeletalMesh>(Mesh))
		{
			SkeletalMesh->Modify();
			SkeletalMesh->SetMoonOutlineMaterialSlots(Slots);
			SkeletalMesh->MarkPackageDirty();
			const int32 NumRefreshed = RefreshComponentsUsing(Mesh);
			Lines.Add(FString::Printf(TEXT("  Wrote %d slots to the mesh asset, and refreshed %d placed component(s)."),
				Slots.Num(), NumRefreshed));
			if (NumRefreshed == 0)
			{
				Lines.Add(TEXT("  (Nothing placed uses this mesh right now, so there was nothing to "
							   "redraw -- the setup still took.)"));
			}
			return;
		}
		Lines.Add(TEXT("  This mesh type has no per-slot outline array on the asset; writing to the "
					   "placed component instead."));
	}

	UMeshComponent* Component = MoonToonMesh::FindPlacedMeshComponent(Mesh);
	if (Component == nullptr)
	{
		Lines.Add(TEXT("  ! No placed component found, and nothing was written. Place the mesh in the "
					   "level, or turn on Write To Mesh Asset."));
		return;
	}

	Component->Modify();
	Component->MaterialSlotsMoonOutlineMaterial.Reset(Slots.Num());
	for (UMaterialInterface* SlotMaterial : Slots)
	{
		Component->MaterialSlotsMoonOutlineMaterial.Add(SlotMaterial);
	}
	Component->MarkRenderStateDirty();
	Lines.Add(FString::Printf(TEXT("  Wrote %d slots to %s."), Slots.Num(), *Component->GetPathName()));
}

FString UMoonToonOutlineSetupTool::Run(const FMoonToonToolContext& Context)
{
	TArray<UObject*> Meshes;
	FString Report;
	if (!ResolveMeshes(Context, Meshes, Report))
	{
		return Report;
	}

	if (OutlineMaterialTemplate == nullptr)
	{
		return TEXT("No Outline Material Template set, so there is nothing to assign.");
	}

	TArray<FString> Lines;
	for (UObject* Mesh : Meshes)
	{
		Lines.Add(FString::Printf(TEXT("%s"), *Mesh->GetPathName()));

		TArray<FSlotPlan> Plan;
		if (!BuildPlan(Mesh, Plan, Lines))
		{
			Lines.Add(TEXT(""));
			continue;
		}

		int32 NumDuplicates = 0;
		int32 NumOutlined = 0;
		for (FSlotPlan& Row : Plan)
		{
			const bool bExcluded =
				Row.Kind == EMoonToonOutlineSectionKind::DuplicateOfAnotherSection ||
				(bExcludeTranslucentSections && Row.Kind == EMoonToonOutlineSectionKind::Translucent);

			NumDuplicates += Row.Kind == EMoonToonOutlineSectionKind::DuplicateOfAnotherSection ? 1 : 0;

			if (!bExcluded && !bPreviewOnly)
			{
				Row.OutlineMaterial = GenerateSlotOutlineMaterial(Mesh, Row, Lines);
			}
			NumOutlined += bExcluded ? 0 : 1;

			FString DupNote;
			if (Row.DuplicateOfMaterialIndex != INDEX_NONE)
			{
				DupNote = FString::Printf(TEXT("  %3.0f%% coincident with slot %d"),
					100.0f * Row.DuplicateFraction, Row.DuplicateOfMaterialIndex);
			}

			Lines.Add(FString::Printf(TEXT("  [%2d] %-22s %-12s %6d tris%s%s"),
				Row.MaterialIndex,
				*Row.SlotName.ToString(),
				KindName(Row.Kind),
				Row.NumFaces,
				*DupNote,
				bExcluded ? TEXT("   (no outline)") : TEXT("   -> OUTLINE")));
		}

		Lines.Add(FString::Printf(TEXT("  %d sections: %d duplicates excluded, %d will draw an outline."),
			Plan.Num(), NumDuplicates, NumOutlined));

		if (bPreviewOnly)
		{
			Lines.Add(TEXT("  Preview only -- nothing was created or written."));
		}
		else
		{
			ApplyPlan(Mesh, Plan, Lines);
		}
		Lines.Add(TEXT(""));
	}

	return FString::Join(Lines, TEXT("\n"));
}

#undef LOCTEXT_NAMESPACE
