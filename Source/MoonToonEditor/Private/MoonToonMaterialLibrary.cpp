// Copyright Dream Moon. All Rights Reserved.

#include "MoonToonMaterialLibrary.h"

#include "MoonToonMaterialPreset.h"

#include "AssetToolsModule.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
// The soft-pointer conversions in the preset code need UTexture complete to see it is a UObject.
#include "Engine/Texture.h"
#include "Factories/MaterialInstanceConstantFactoryNew.h"
#include "IAssetTools.h"
#include "MaterialEditingLibrary.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialInterface.h"
#include "Misc/PackageName.h"
#include "Modules/ModuleManager.h"
#include "ScopedTransaction.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

#define LOCTEXT_NAMESPACE "MoonToonMaterialLibrary"

DEFINE_LOG_CATEGORY_STATIC(LogMoonToonMaterial, Log, All);

namespace MoonToonMaterial
{
	TArrayView<const EMaterialParameterType> GetEditableTypes()
	{
		static const EMaterialParameterType Types[] =
		{
			EMaterialParameterType::Scalar,
			EMaterialParameterType::Vector,
			EMaterialParameterType::Texture,
			EMaterialParameterType::StaticSwitch,
		};
		return MakeArrayView(Types);
	}

	bool IsOverriddenHere(UMaterialInstanceConstant* Instance, EMaterialParameterType Type, const FMaterialParameterInfo& Info)
	{
		if (!Instance)
		{
			return false;
		}
		FMaterialParameterMetadata Meta;
		return Instance->GetParameterValue(Type, Info, Meta, EMaterialGetParameterValueFlags::CheckInstanceOverrides);
	}

	bool ExistsInParent(UMaterialInstanceConstant* Instance, EMaterialParameterType Type, const FMaterialParameterInfo& Info)
	{
		if (!Instance)
		{
			return false;
		}
		FMaterialParameterMetadata Meta;
		// CheckNonOverrides walks the parent chain's own values and skips this instance's overrides,
		// which is exactly the question "would this parameter still exist if I dropped my override".
		return Instance->GetParameterValue(Type, Info, Meta, EMaterialGetParameterValueFlags::CheckNonOverrides);
	}

	bool ValuesEqual(const FMaterialParameterValue& A, const FMaterialParameterValue& B)
	{
		if (A.Type != B.Type)
		{
			return false;
		}
		switch (A.Type)
		{
		case EMaterialParameterType::Scalar:
			return A.AsScalar() == B.AsScalar();
		case EMaterialParameterType::Vector:
			return A.AsLinearColor() == B.AsLinearColor();
		case EMaterialParameterType::Texture:
			return A.Texture == B.Texture;
		case EMaterialParameterType::StaticSwitch:
			return A.AsStaticSwitch() == B.AsStaticSwitch();
		default:
			return false;
		}
	}

	void GatherSharedParameters(
		const TArray<UMaterialInstanceConstant*>& Instances,
		TArray<FMoonToonMaterialParam>& OutParams,
		int32& OutSkippedCount)
	{
		OutParams.Reset();
		OutSkippedCount = 0;

		TArray<UMaterialInstanceConstant*> Valid;
		for (UMaterialInstanceConstant* Instance : Instances)
		{
			if (Instance)
			{
				Valid.AddUnique(Instance);
			}
		}
		if (Valid.Num() == 0)
		{
			return;
		}

		for (const EMaterialParameterType Type : GetEditableTypes())
		{
			// The first instance seeds the row set; every other one can only narrow it.
			TMap<FMaterialParameterInfo, FMaterialParameterMetadata> Seed;
			Valid[0]->GetAllParametersOfType(Type, Seed);

			for (const TPair<FMaterialParameterInfo, FMaterialParameterMetadata>& Pair : Seed)
			{
				FMoonToonMaterialParam Param;
				Param.Info = Pair.Key;
				Param.Type = Type;
				Param.Group = Pair.Value.Group;
				Param.SortPriority = Pair.Value.SortPriority;
				Param.Description = FText::FromString(Pair.Value.Description);
				Param.ScalarMin = Pair.Value.ScalarMin;
				Param.ScalarMax = Pair.Value.ScalarMax;
				Param.bUsedAsChannelMask = Pair.Value.bUsedAsChannelMask;
				Param.Value = Pair.Value.Value;
				Param.NumInstances = Valid.Num();
				Param.NumOverriding = IsOverriddenHere(Valid[0], Type, Param.Info) ? 1 : 0;

				bool bOnAll = true;
				for (int32 Index = 1; Index < Valid.Num(); ++Index)
				{
					FMaterialParameterMetadata Other;
					if (!Valid[Index]->GetParameterValue(Type, Param.Info, Other))
					{
						bOnAll = false;
						break;
					}
					if (Param.bSameValue && !ValuesEqual(Param.Value, Other.Value))
					{
						Param.bSameValue = false;
					}
					if (IsOverriddenHere(Valid[Index], Type, Param.Info))
					{
						++Param.NumOverriding;
					}
				}

				if (bOnAll)
				{
					OutParams.Add(MoveTemp(Param));
				}
				else
				{
					++OutSkippedCount;
				}
			}
		}

		// Group first, then the material's own sort priority, then name -- the same order the material
		// instance editor uses, so muscle memory carries over.
		OutParams.Sort([](const FMoonToonMaterialParam& A, const FMoonToonMaterialParam& B)
		{
			if (A.Group != B.Group)
			{
				return A.Group.LexicalLess(B.Group);
			}
			if (A.SortPriority != B.SortPriority)
			{
				return A.SortPriority < B.SortPriority;
			}
			return A.Info.Name.LexicalLess(B.Info.Name);
		});
	}

	bool ApplyValue(
		UMaterialInstanceConstant* Instance,
		EMaterialParameterType Type,
		const FMaterialParameterInfo& Info,
		const FMaterialParameterValue& Value)
	{
		if (!Instance)
		{
			return false;
		}

		switch (Type)
		{
		case EMaterialParameterType::Scalar:
			Instance->SetScalarParameterValueEditorOnly(Info, Value.AsScalar());
			return true;
		case EMaterialParameterType::Vector:
			Instance->SetVectorParameterValueEditorOnly(Info, Value.AsLinearColor());
			return true;
		case EMaterialParameterType::Texture:
			Instance->SetTextureParameterValueEditorOnly(Info, Value.Texture);
			return true;
		case EMaterialParameterType::StaticSwitch:
		{
			// NOT SetStaticSwitchParameterValueEditorOnly: that one appends an entry with a zero
			// ExpressionGuid, and the static permutation update then fails to match it against the
			// parent's expression and leaves a second, junk entry behind under a mangled
			// "<name>_<number>" name -- which serialises into the asset. Measured on both a transient
			// instance and MI_Moon_Toon_VRM_Base.
			//
			// Going through the engine's update context carries the parent's GUID across, and its
			// destructor is what publishes the recompile (so a switch write always costs one).
			FMaterialParameterMetadata Meta;
			if (!Instance->GetParameterValue(Type, Info, Meta))
			{
				return false;
			}
			Meta.Value = Value;
			Meta.bOverride = true;

			FMaterialInstanceParameterUpdateContext UpdateContext(Instance);
			UpdateContext.SetParameterValueEditorOnly(Info, Meta);
			return true;
		}
		default:
			return false;
		}
	}

	bool ClearOverride(UMaterialInstanceConstant* Instance, EMaterialParameterType Type, const FMaterialParameterInfo& Info)
	{
		if (!Instance)
		{
			return false;
		}

		auto RemoveFrom = [&Info](auto& ParameterArray)
		{
			const int32 Index = ParameterArray.IndexOfByPredicate(
				[&Info](const auto& Param) { return Param.ParameterInfo == Info; });
			if (Index != INDEX_NONE)
			{
				ParameterArray.RemoveAt(Index);
				return true;
			}
			return false;
		};

		switch (Type)
		{
		case EMaterialParameterType::Scalar:
			return RemoveFrom(Instance->ScalarParameterValues);
		case EMaterialParameterType::Vector:
			return RemoveFrom(Instance->VectorParameterValues);
		case EMaterialParameterType::Texture:
			return RemoveFrom(Instance->TextureParameterValues);
		case EMaterialParameterType::StaticSwitch:
			// The static parameter set is private to the engine (UMaterialEditingLibrary is a friend
			// of UMaterialInstance, this module is not), and clearing a switch means un-flagging the
			// entry rather than removing it. Go through the engine's own entry point, which also
			// means this one publishes immediately -- a static switch has to recompile anyway.
			return UMaterialEditingLibrary::SetMaterialInstanceParameterOverride(
				Instance, Info.Name, /*bOverride=*/false, Info.Association);
		default:
			return false;
		}
	}

	void FinishEdits(const TArray<UMaterialInstanceConstant*>& Instances)
	{
		for (UMaterialInstanceConstant* Instance : Instances)
		{
			if (Instance)
			{
				// Marks the package dirty, rebuilds, and recompiles the static permutation when a
				// switch moved. One call per instance per user action, never per parameter.
				UMaterialEditingLibrary::UpdateMaterialInstance(Instance);
			}
		}
	}

	FOnReport& OnReport()
	{
		static FOnReport Delegate;
		return Delegate;
	}

	void PreviewEdits(const TArray<UMaterialInstanceConstant*>& Instances)
	{
		for (UMaterialInstanceConstant* Instance : Instances)
		{
			if (Instance)
			{
				// Re-reads the parameter arrays into the uniform expression cache. Enough to see a
				// slider move in the viewport; nothing is dirtied and no shader is touched, so this
				// is safe to run on every drag tick.
				Instance->RecacheUniformExpressions(false);
			}
		}
	}
}

namespace
{
	/** Instances minus the nulls and duplicates, which both menus and Python hand us regularly. */
	TArray<UMaterialInstanceConstant*> Sanitize(const TArray<UMaterialInstanceConstant*>& Instances)
	{
		TArray<UMaterialInstanceConstant*> Result;
		for (UMaterialInstanceConstant* Instance : Instances)
		{
			if (Instance)
			{
				Result.AddUnique(Instance);
			}
		}
		return Result;
	}

	/** Every override the instance itself carries, as (type, info) pairs. */
	void CollectOverrides(UMaterialInstanceConstant* Instance, TArray<TPair<EMaterialParameterType, FMaterialParameterInfo>>& Out)
	{
		for (const EMaterialParameterType Type : MoonToonMaterial::GetEditableTypes())
		{
			TMap<FMaterialParameterInfo, FMaterialParameterMetadata> Params;
			Instance->GetAllParametersOfType(Type, Params);
			for (const TPair<FMaterialParameterInfo, FMaterialParameterMetadata>& Pair : Params)
			{
				if (MoonToonMaterial::IsOverriddenHere(Instance, Type, Pair.Key))
				{
					Out.Emplace(Type, Pair.Key);
				}
			}
		}

		// GetAllParametersOfType only reports what the parent chain declares, so an override left
		// behind by a re-parent is invisible to it -- and those are exactly the ones worth finding.
		// Read them straight off the instance's own arrays instead.
		for (const FScalarParameterValue& Param : Instance->ScalarParameterValues)
		{
			Out.AddUnique(TPair<EMaterialParameterType, FMaterialParameterInfo>(EMaterialParameterType::Scalar, Param.ParameterInfo));
		}
		for (const FVectorParameterValue& Param : Instance->VectorParameterValues)
		{
			Out.AddUnique(TPair<EMaterialParameterType, FMaterialParameterInfo>(EMaterialParameterType::Vector, Param.ParameterInfo));
		}
		for (const FTextureParameterValue& Param : Instance->TextureParameterValues)
		{
			Out.AddUnique(TPair<EMaterialParameterType, FMaterialParameterInfo>(EMaterialParameterType::Texture, Param.ParameterInfo));
		}
		for (const FStaticSwitchParameter& Param : Instance->GetStaticParameters().StaticSwitchParameters)
		{
			if (Param.bOverride)
			{
				Out.AddUnique(TPair<EMaterialParameterType, FMaterialParameterInfo>(EMaterialParameterType::StaticSwitch, Param.ParameterInfo));
			}
		}
	}

	const TCHAR* TypeName(EMaterialParameterType Type)
	{
		switch (Type)
		{
		case EMaterialParameterType::Scalar:       return TEXT("scalar");
		case EMaterialParameterType::Vector:       return TEXT("vector");
		case EMaterialParameterType::Texture:      return TEXT("texture");
		case EMaterialParameterType::StaticSwitch: return TEXT("switch");
		default:                                   return TEXT("?");
		}
	}

	/**
	 * What the names have in common, cut back to an underscore.
	 *
	 * Used to turn a set of preset parents into short variant suffixes: given
	 * MI_Moon_Toon_VRM_TwoSide and MI_Moon_Toon_VRM_Masked_TwoSide it yields MI_Moon_Toon_VRM_, so the
	 * masters end up named ..._Master_TwoSide and ..._Master_Masked_TwoSide rather than repeating the
	 * whole preset name. Cutting at an underscore keeps the remainder a whole word.
	 */
	FString CommonNamePrefix(const TArray<FString>& Names)
	{
		if (Names.Num() < 2)
		{
			return FString();
		}

		int32 PrefixLength = Names[0].Len();
		for (int32 Index = 1; Index < Names.Num(); ++Index)
		{
			const FString& Other = Names[Index];
			int32 Common = 0;
			while (Common < PrefixLength && Common < Other.Len() && Names[0][Common] == Other[Common])
			{
				++Common;
			}
			PrefixLength = Common;
		}

		FString Prefix = Names[0].Left(PrefixLength);
		int32 LastUnderscore = INDEX_NONE;
		if (Prefix.FindLastChar(TEXT('_'), LastUnderscore))
		{
			return Prefix.Left(LastUnderscore + 1);
		}
		return FString();
	}

	/** The value this instance itself carries for a parameter, or false when it does not override it. */
	bool GetOverrideValue(
		UMaterialInstanceConstant* Instance,
		EMaterialParameterType Type,
		const FMaterialParameterInfo& Info,
		FMaterialParameterValue& OutValue)
	{
		FMaterialParameterMetadata Meta;
		if (!Instance || !Instance->GetParameterValue(Type, Info, Meta, EMaterialGetParameterValueFlags::CheckInstanceOverrides))
		{
			return false;
		}
		OutValue = Meta.Value;
		return true;
	}

	/** Content path of the folder an asset lives in, e.g. /Game/Characters/Lin. */
	FString FolderOf(const UObject* Asset)
	{
		return Asset ? FPackageName::GetLongPackagePath(Asset->GetOutermost()->GetName()) : FString();
	}

	/** The folder most of these instances live in. New masters belong with the family, not elsewhere. */
	FString MostCommonFolder(const TArray<UMaterialInstanceConstant*>& Instances)
	{
		TMap<FString, int32> Counts;
		for (const UMaterialInstanceConstant* Instance : Instances)
		{
			const FString Folder = FolderOf(Instance);
			if (!Folder.IsEmpty())
			{
				Counts.FindOrAdd(Folder)++;
			}
		}

		FString Best;
		int32 BestCount = 0;
		for (const TPair<FString, int32>& Pair : Counts)
		{
			if (Pair.Value > BestCount)
			{
				Best = Pair.Key;
				BestCount = Pair.Value;
			}
		}
		return Best;
	}

	/** A slot name turned into something that can be an asset name. */
	FString SanitizeAssetName(const FString& In)
	{
		FString Out = In;
		// Object names reject these outright; the CJK slot names a VRM brings in are fine as they are.
		for (const TCHAR Bad : FString(INVALID_OBJECTNAME_CHARACTERS))
		{
			Out.ReplaceCharInline(Bad, TEXT('_'));
		}
		Out.TrimStartAndEndInline();
		return Out;
	}

	/** The mesh's material slots, whichever mesh class it is. */
	bool GetMeshMaterials(UObject* Mesh, TArray<TPair<FName, UMaterialInterface*>>& Out)
	{
		Out.Reset();
		if (const USkeletalMesh* SkeletalMesh = Cast<USkeletalMesh>(Mesh))
		{
			for (const FSkeletalMaterial& Material : SkeletalMesh->GetMaterials())
			{
				Out.Emplace(Material.MaterialSlotName, Material.MaterialInterface);
			}
			return true;
		}
		if (const UStaticMesh* StaticMesh = Cast<UStaticMesh>(Mesh))
		{
			for (const FStaticMaterial& Material : StaticMesh->GetStaticMaterials())
			{
				Out.Emplace(Material.MaterialSlotName, Material.MaterialInterface);
			}
			return true;
		}
		return false;
	}

	/** Writes one slot. The caller is expected to have opened a transaction and called Modify. */
	bool SetMeshMaterial(UObject* Mesh, int32 SlotIndex, UMaterialInterface* Material)
	{
		if (USkeletalMesh* SkeletalMesh = Cast<USkeletalMesh>(Mesh))
		{
			TArray<FSkeletalMaterial> Materials = SkeletalMesh->GetMaterials();
			if (!Materials.IsValidIndex(SlotIndex))
			{
				return false;
			}
			Materials[SlotIndex].MaterialInterface = Material;
			SkeletalMesh->SetMaterials(Materials);
			return true;
		}
		if (UStaticMesh* StaticMesh = Cast<UStaticMesh>(Mesh))
		{
			TArray<FStaticMaterial> Materials = StaticMesh->GetStaticMaterials();
			if (!Materials.IsValidIndex(SlotIndex))
			{
				return false;
			}
			Materials[SlotIndex].MaterialInterface = Material;
			StaticMesh->SetStaticMaterials(Materials);
			return true;
		}
		return false;
	}

	/** Finds the instance already at this path, or makes an empty one there. Null on failure. */
	UMaterialInstanceConstant* FindOrCreateInstance(const FString& Folder, const FString& AssetName)
	{
		const FString ObjectPath = FString::Printf(TEXT("%s/%s.%s"), *Folder, *AssetName, *AssetName);
		if (FPackageName::DoesPackageExist(FString::Printf(TEXT("%s/%s"), *Folder, *AssetName)))
		{
			// Re-running the operation must land on the master it made last time, not a second one.
			return LoadObject<UMaterialInstanceConstant>(nullptr, *ObjectPath);
		}

		IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
		UMaterialInstanceConstantFactoryNew* Factory = NewObject<UMaterialInstanceConstantFactoryNew>();
		return Cast<UMaterialInstanceConstant>(AssetTools.CreateAsset(
			AssetName, Folder, UMaterialInstanceConstant::StaticClass(), Factory));
	}
}

FString UMoonToonMaterialLibrary::SetParentOnInstances(
	const TArray<UMaterialInstanceConstant*>& Instances,
	UMaterialInterface* NewParent,
	bool bClearOrphanedOverrides)
{
	const TArray<UMaterialInstanceConstant*> Targets = Sanitize(Instances);
	if (Targets.Num() == 0)
	{
		return TEXT("No material instances given.");
	}
	if (!NewParent)
	{
		return TEXT("No parent given.");
	}

	FString Report = FString::Printf(TEXT("Set parent to %s\n\n"), *NewParent->GetName());
	int32 NumChanged = 0;

	for (UMaterialInstanceConstant* Instance : Targets)
	{
		// A material cannot be its own ancestor; the engine would recurse forever resolving values.
		if (Instance == NewParent || NewParent->IsDependent(Instance))
		{
			Report += FString::Printf(TEXT("  %-40s SKIPPED -- that parent descends from this instance\n"), *Instance->GetName());
			continue;
		}

		const UMaterialInterface* OldParent = Instance->Parent;
		if (OldParent == NewParent)
		{
			Report += FString::Printf(TEXT("  %-40s already had this parent\n"), *Instance->GetName());
			continue;
		}

		Instance->SetParentEditorOnly(NewParent);
		++NumChanged;

		// Now that the parent moved, whatever the instance still overrides is either carried over or
		// orphaned. Name both, because "my override stopped doing anything" is the failure mode of a
		// re-parent and it is otherwise completely silent.
		TArray<TPair<EMaterialParameterType, FMaterialParameterInfo>> Overrides;
		CollectOverrides(Instance, Overrides);

		TArray<FString> Orphans;
		int32 NumKept = 0;
		for (const TPair<EMaterialParameterType, FMaterialParameterInfo>& Override : Overrides)
		{
			if (MoonToonMaterial::ExistsInParent(Instance, Override.Key, Override.Value))
			{
				++NumKept;
			}
			else
			{
				Orphans.Add(FString::Printf(TEXT("%s (%s)"), *Override.Value.Name.ToString(), TypeName(Override.Key)));
				if (bClearOrphanedOverrides)
				{
					MoonToonMaterial::ClearOverride(Instance, Override.Key, Override.Value);
				}
			}
		}

		Report += FString::Printf(TEXT("  %-40s %s -> %s, %d override(s) kept"),
			*Instance->GetName(),
			OldParent ? *OldParent->GetName() : TEXT("<none>"),
			*NewParent->GetName(),
			NumKept);

		if (Orphans.Num() > 0)
		{
			Report += FString::Printf(TEXT(", %d %s: %s"),
				Orphans.Num(),
				bClearOrphanedOverrides ? TEXT("cleared") : TEXT("now dead"),
				*FString::Join(Orphans, TEXT(", ")));
		}
		Report += TEXT("\n");
	}

	MoonToonMaterial::FinishEdits(Targets);

	Report += FString::Printf(TEXT("\n%d of %d instance(s) re-parented. Save them to keep it.\n"),
		NumChanged, Targets.Num());
	UE_LOG(LogMoonToonMaterial, Log, TEXT("[MoonToon] SetParentOnInstances: %d/%d -> %s"),
		NumChanged, Targets.Num(), *NewParent->GetName());
	return Report;
}

FString UMoonToonMaterialLibrary::InsertCharacterMasters(
	const TArray<UMaterialInstanceConstant*>& Instances,
	const FString& CharacterName,
	const FString& DestinationFolder,
	bool bPromoteCommonOverrides,
	bool bPromoteStaticSwitches)
{
	const TArray<UMaterialInstanceConstant*> Targets = Sanitize(Instances);
	if (Targets.Num() == 0)
	{
		return TEXT("No material instances given.");
	}
	if (CharacterName.IsEmpty())
	{
		return TEXT("No character name given.");
	}

	const FString Folder = DestinationFolder.IsEmpty() ? MostCommonFolder(Targets) : DestinationFolder;
	if (Folder.IsEmpty())
	{
		return TEXT("Could not work out where to put the master; give a destination folder.");
	}

	// Group by the parent each instance has right now. That is what a master can stand in for: every
	// member of a group resolves the same parameters, so re-parenting them under a fresh child of
	// that parent changes nothing until the master itself is edited.
	TMap<UMaterialInterface*, TArray<UMaterialInstanceConstant*>> Groups;
	FString Report;
	for (UMaterialInstanceConstant* Instance : Targets)
	{
		if (UMaterialInterface* Parent = Instance->Parent)
		{
			Groups.FindOrAdd(Parent).Add(Instance);
		}
		else
		{
			Report += FString::Printf(TEXT("  %-40s SKIPPED -- no parent to insert under\n"), *Instance->GetName());
		}
	}

	TArray<FString> ParentNames;
	for (const TPair<UMaterialInterface*, TArray<UMaterialInstanceConstant*>>& Group : Groups)
	{
		ParentNames.Add(Group.Key->GetName());
	}
	const FString SharedPrefix = CommonNamePrefix(ParentNames);

	TArray<UMaterialInstanceConstant*> Touched;
	int32 NumMasters = 0;
	int32 NumReparented = 0;
	int32 NumPromoted = 0;
	int32 NumSwitchesLeft = 0;

	for (const TPair<UMaterialInterface*, TArray<UMaterialInstanceConstant*>>& Group : Groups)
	{
		UMaterialInterface* GroupParent = Group.Key;
		const TArray<UMaterialInstanceConstant*>& Children = Group.Value;

		// One preset in play means the plain name; several mean each master says which it belongs to.
		FString MasterName = FString::Printf(TEXT("MI_%s_Master"), *CharacterName);
		if (Groups.Num() > 1)
		{
			const FString Variant = GroupParent->GetName().RightChop(SharedPrefix.Len());
			MasterName += TEXT("_") + Variant;
		}

		if (GroupParent->GetName() == MasterName)
		{
			Report += FString::Printf(TEXT("  %-40s already under it (%d instance(s))\n"),
				*MasterName, Children.Num());
			continue;
		}

		UMaterialInstanceConstant* Master = FindOrCreateInstance(Folder, MasterName);
		if (!Master)
		{
			Report += FString::Printf(TEXT("  %-40s FAILED to create in %s\n"), *MasterName, *Folder);
			continue;
		}

		if (Master->Parent && Master->Parent != GroupParent)
		{
			// Something else already owns that name. Renaming or re-parenting it would be a guess.
			Report += FString::Printf(TEXT("  %-40s SKIPPED -- already exists under a different parent (%s)\n"),
				*MasterName, *Master->Parent->GetName());
			continue;
		}
		if (!Master->Parent)
		{
			Master->SetParentEditorOnly(GroupParent);
			++NumMasters;
		}
		Touched.Add(Master);

		for (UMaterialInstanceConstant* Child : Children)
		{
			if (Child == Master)
			{
				continue;
			}
			Child->SetParentEditorOnly(Master);
			Touched.Add(Child);
			++NumReparented;
		}

		Report += FString::Printf(TEXT("  %-40s <- %s, %d instance(s) re-parented\n"),
			*MasterName, *GroupParent->GetName(), Children.Num());

		if (!bPromoteCommonOverrides)
		{
			continue;
		}

		// An override every child carries with the same value is a statement about the character, not
		// about one section of it. Move it up, and take it off the children so editing the master is
		// what changes them from now on -- leaving copies behind would make the master look wired up
		// while doing nothing.
		TMap<TPair<EMaterialParameterType, FMaterialParameterInfo>, TPair<int32, FMaterialParameterValue>> Candidates;
		for (UMaterialInstanceConstant* Child : Children)
		{
			TArray<TPair<EMaterialParameterType, FMaterialParameterInfo>> Overrides;
			CollectOverrides(Child, Overrides);
			for (const TPair<EMaterialParameterType, FMaterialParameterInfo>& Override : Overrides)
			{
				if (Override.Key == EMaterialParameterType::StaticSwitch && !bPromoteStaticSwitches)
				{
					++NumSwitchesLeft;
					continue;
				}
				// An override the parent chain does not declare is dead weight; promoting it would
				// only plant the same dead weight one level up.
				if (!MoonToonMaterial::ExistsInParent(Child, Override.Key, Override.Value))
				{
					continue;
				}

				FMaterialParameterValue Value;
				if (!GetOverrideValue(Child, Override.Key, Override.Value, Value))
				{
					continue;
				}

				if (TPair<int32, FMaterialParameterValue>* Existing = Candidates.Find(Override))
				{
					// A disagreement drops it out of the running for good, hence the -1 rather than
					// just not counting this child.
					if (Existing->Key >= 0 && MoonToonMaterial::ValuesEqual(Existing->Value, Value))
					{
						++Existing->Key;
					}
					else
					{
						Existing->Key = -1;
					}
				}
				else
				{
					Candidates.Add(Override, TPair<int32, FMaterialParameterValue>(1, Value));
				}
			}
		}

		TArray<FString> Promoted;
		for (const TPair<TPair<EMaterialParameterType, FMaterialParameterInfo>, TPair<int32, FMaterialParameterValue>>& Candidate : Candidates)
		{
			if (Candidate.Value.Key != Children.Num())
			{
				continue;
			}

			const EMaterialParameterType Type = Candidate.Key.Key;
			const FMaterialParameterInfo& Info = Candidate.Key.Value;
			if (!MoonToonMaterial::ApplyValue(Master, Type, Info, Candidate.Value.Value))
			{
				continue;
			}

			for (UMaterialInstanceConstant* Child : Children)
			{
				MoonToonMaterial::ClearOverride(Child, Type, Info);
			}
			Promoted.Add(FString::Printf(TEXT("%s (%s)"), *Info.Name.ToString(), TypeName(Type)));
		}

		NumPromoted += Promoted.Num();
		if (Promoted.Num() > 0)
		{
			Promoted.Sort();
			Report += FString::Printf(TEXT("      promoted %d shared override(s): %s\n"),
				Promoted.Num(), *FString::Join(Promoted, TEXT(", ")));
		}
	}

	MoonToonMaterial::FinishEdits(Touched);

	FString Header = FString::Printf(
		TEXT("Character master for '%s' in %s\n\n"), *CharacterName, *Folder);
	FString Footer = FString::Printf(
		TEXT("\n%d master(s) created, %d instance(s) re-parented, %d override(s) promoted.\n"),
		NumMasters, NumReparented, NumPromoted);
	if (NumSwitchesLeft > 0)
	{
		Footer += FString::Printf(
			TEXT("%d static-switch override(s) left on the children. Promoting a switch republishes and\n"
				 "recompiles each instance one at a time, so it is off unless asked for.\n"),
			NumSwitchesLeft);
	}
	Footer += TEXT("Nothing is saved yet, and a new master only survives once its package is written.\n");

	UE_LOG(LogMoonToonMaterial, Log, TEXT("[MoonToon] InsertCharacterMasters '%s': %d master(s), %d re-parented, %d promoted"),
		*CharacterName, NumMasters, NumReparented, NumPromoted);
	return Header + Report + Footer;
}

FString UMoonToonMaterialLibrary::AssignMaterialToMeshSections(
	UObject* Mesh,
	const TArray<int32>& MaterialIndices,
	UMaterialInterface* Material)
{
	TArray<TPair<FName, UMaterialInterface*>> Slots;
	if (!GetMeshMaterials(Mesh, Slots))
	{
		return TEXT("Not a static or skeletal mesh.");
	}

	const FScopedTransaction Transaction(LOCTEXT("AssignMaterialTransaction", "Assign Material to Sections"));
	Mesh->Modify();

	FString Report;
	int32 NumChanged = 0;
	for (int32 SlotIndex = 0; SlotIndex < Slots.Num(); ++SlotIndex)
	{
		if (MaterialIndices.Num() > 0 && !MaterialIndices.Contains(SlotIndex))
		{
			continue;
		}
		if (Slots[SlotIndex].Value == Material)
		{
			continue;
		}

		const UMaterialInterface* Old = Slots[SlotIndex].Value;
		if (SetMeshMaterial(Mesh, SlotIndex, Material))
		{
			++NumChanged;
			Report += FString::Printf(TEXT("  [%2d] %-24s %s -> %s\n"),
				SlotIndex,
				*Slots[SlotIndex].Key.ToString(),
				Old ? *Old->GetName() : TEXT("<none>"),
				Material ? *Material->GetName() : TEXT("<none>"));
		}
	}

	Mesh->PostEditChange();
	Mesh->MarkPackageDirty();

	return FString::Printf(TEXT("Assigned %s to %d slot(s) on %s.\n\n%s"),
		Material ? *Material->GetName() : TEXT("<none>"),
		NumChanged,
		*Mesh->GetName(),
		*Report);
}

FString UMoonToonMaterialLibrary::CreateInstancesForMeshSections(
	UObject* Mesh,
	const TArray<int32>& MaterialIndices,
	UMaterialInterface* Parent,
	const FString& DestinationFolder,
	bool bReplaceExistingInstances)
{
	TArray<TPair<FName, UMaterialInterface*>> Slots;
	if (!GetMeshMaterials(Mesh, Slots))
	{
		return TEXT("Not a static or skeletal mesh.");
	}
	if (!Parent)
	{
		return TEXT("No parent material given.");
	}

	const FString Folder = DestinationFolder.IsEmpty() ? FolderOf(Mesh) : DestinationFolder;
	if (Folder.IsEmpty())
	{
		return TEXT("Could not work out where to put the instances; give a destination folder.");
	}

	const FScopedTransaction Transaction(LOCTEXT("CreateInstancesTransaction", "Create Section Material Instances"));
	Mesh->Modify();

	FString Report;
	TArray<UMaterialInstanceConstant*> Created;
	int32 NumSkipped = 0;

	for (int32 SlotIndex = 0; SlotIndex < Slots.Num(); ++SlotIndex)
	{
		if (MaterialIndices.Num() > 0 && !MaterialIndices.Contains(SlotIndex))
		{
			continue;
		}

		UMaterialInterface* Existing = Slots[SlotIndex].Value;
		if (!bReplaceExistingInstances && Existing && Existing->IsA<UMaterialInstanceConstant>())
		{
			++NumSkipped;
			continue;
		}

		// Named after the slot, because that is the name the artist sees in the section list and in
		// the DCC. Falls back to the index when a slot has no name at all.
		FString AssetName = SanitizeAssetName(Slots[SlotIndex].Key.ToString());
		if (AssetName.IsEmpty() || Slots[SlotIndex].Key.IsNone())
		{
			AssetName = FString::Printf(TEXT("%s_%d"), *Mesh->GetName(), SlotIndex);
		}
		AssetName = FString::Printf(TEXT("MI_%s"), *AssetName);

		UMaterialInstanceConstant* Instance = FindOrCreateInstance(Folder, AssetName);
		if (!Instance)
		{
			Report += FString::Printf(TEXT("  [%2d] %-24s FAILED to create %s\n"),
				SlotIndex, *Slots[SlotIndex].Key.ToString(), *AssetName);
			continue;
		}

		// Re-using an asset that happens to sit at that path is only safe when it is already this
		// parent's child; anything else and the slot would silently inherit somebody else's setup.
		if (Instance->Parent && Instance->Parent != Parent)
		{
			Report += FString::Printf(TEXT("  [%2d] %-24s SKIPPED -- %s exists with parent %s\n"),
				SlotIndex, *Slots[SlotIndex].Key.ToString(), *AssetName, *Instance->Parent->GetName());
			continue;
		}
		if (!Instance->Parent)
		{
			Instance->SetParentEditorOnly(Parent);
		}

		SetMeshMaterial(Mesh, SlotIndex, Instance);
		Created.AddUnique(Instance);
		Report += FString::Printf(TEXT("  [%2d] %-24s %s -> %s\n"),
			SlotIndex,
			*Slots[SlotIndex].Key.ToString(),
			Existing ? *Existing->GetName() : TEXT("<none>"),
			*AssetName);
	}

	MoonToonMaterial::FinishEdits(Created);
	Mesh->PostEditChange();
	Mesh->MarkPackageDirty();

	FString Footer = FString::Printf(TEXT("\n%d instance(s) under %s in %s."),
		Created.Num(), *Parent->GetName(), *Folder);
	if (NumSkipped > 0)
	{
		Footer += FString::Printf(TEXT("\n%d slot(s) already used an instance and were left alone."), NumSkipped);
	}
	Footer += TEXT("\nNothing is saved yet.\n");

	return FString::Printf(TEXT("Create instances for %s\n\n%s%s"), *Mesh->GetName(), *Report, *Footer);
}

FString UMoonToonMaterialLibrary::ClearOrphanedOverrides(const TArray<UMaterialInstanceConstant*>& Instances)
{
	const TArray<UMaterialInstanceConstant*> Targets = Sanitize(Instances);
	if (Targets.Num() == 0)
	{
		return TEXT("No material instances given.");
	}

	FString Report;
	int32 TotalCleared = 0;

	for (UMaterialInstanceConstant* Instance : Targets)
	{
		TArray<TPair<EMaterialParameterType, FMaterialParameterInfo>> Overrides;
		CollectOverrides(Instance, Overrides);

		TArray<FString> Cleared;
		for (const TPair<EMaterialParameterType, FMaterialParameterInfo>& Override : Overrides)
		{
			if (!MoonToonMaterial::ExistsInParent(Instance, Override.Key, Override.Value)
				&& MoonToonMaterial::ClearOverride(Instance, Override.Key, Override.Value))
			{
				Cleared.Add(FString::Printf(TEXT("%s (%s)"), *Override.Value.Name.ToString(), TypeName(Override.Key)));
			}
		}

		TotalCleared += Cleared.Num();
		Report += FString::Printf(TEXT("  %-40s %d cleared%s%s\n"),
			*Instance->GetName(),
			Cleared.Num(),
			Cleared.Num() > 0 ? TEXT(": ") : TEXT(""),
			*FString::Join(Cleared, TEXT(", ")));
	}

	MoonToonMaterial::FinishEdits(Targets);
	return FString::Printf(TEXT("Cleared %d orphaned override(s) across %d instance(s).\n\n%s"),
		TotalCleared, Targets.Num(), *Report);
}

FString UMoonToonMaterialLibrary::CopyOverrides(
	UMaterialInstanceConstant* Source,
	const TArray<UMaterialInstanceConstant*>& Targets)
{
	if (!Source)
	{
		return TEXT("No source instance given.");
	}

	TArray<UMaterialInstanceConstant*> Written = Sanitize(Targets);
	Written.Remove(Source);
	if (Written.Num() == 0)
	{
		return TEXT("No target instances given.");
	}

	TArray<TPair<EMaterialParameterType, FMaterialParameterInfo>> Overrides;
	CollectOverrides(Source, Overrides);
	if (Overrides.Num() == 0)
	{
		return FString::Printf(TEXT("%s overrides nothing; nothing to copy.\n"), *Source->GetName());
	}

	FString Report = FString::Printf(TEXT("Copy %d override(s) from %s\n\n"), Overrides.Num(), *Source->GetName());

	for (UMaterialInstanceConstant* Target : Written)
	{
		int32 NumCopied = 0;
		TArray<FString> Missing;
		for (const TPair<EMaterialParameterType, FMaterialParameterInfo>& Override : Overrides)
		{
			FMaterialParameterMetadata Meta;
			if (!Source->GetParameterValue(Override.Key, Override.Value, Meta))
			{
				continue;
			}
			// Only what the target's own parent declares: writing anything else would just plant a
			// fresh orphan on the target.
			if (!MoonToonMaterial::ExistsInParent(Target, Override.Key, Override.Value))
			{
				Missing.Add(Override.Value.Name.ToString());
				continue;
			}
			if (MoonToonMaterial::ApplyValue(Target, Override.Key, Override.Value, Meta.Value))
			{
				++NumCopied;
			}
		}

		Report += FString::Printf(TEXT("  %-40s %d copied"), *Target->GetName(), NumCopied);
		if (Missing.Num() > 0)
		{
			Report += FString::Printf(TEXT(", %d not in its parent: %s"), Missing.Num(), *FString::Join(Missing, TEXT(", ")));
		}
		Report += TEXT("\n");
	}

	MoonToonMaterial::FinishEdits(Written);
	return Report;
}

namespace
{
	/** Shared body of the two batch setters: same skip rule, same report shape. */
	FString SetOnInstances(
		const TArray<UMaterialInstanceConstant*>& Instances,
		EMaterialParameterType Type,
		const FName ParameterName,
		const FMaterialParameterValue& Value,
		const TCHAR* ValueText)
	{
		const TArray<UMaterialInstanceConstant*> Targets = Sanitize(Instances);
		if (Targets.Num() == 0)
		{
			return TEXT("No material instances given.");
		}

		const FMaterialParameterInfo Info(ParameterName);
		TArray<UMaterialInstanceConstant*> Written;
		TArray<FString> Skipped;

		for (UMaterialInstanceConstant* Instance : Targets)
		{
			// Writing a parameter the parent never declares would create exactly the dead override
			// that ClearOrphanedOverrides exists to remove.
			if (!MoonToonMaterial::ExistsInParent(Instance, Type, Info)
				&& !MoonToonMaterial::IsOverriddenHere(Instance, Type, Info))
			{
				Skipped.Add(Instance->GetName());
				continue;
			}
			if (MoonToonMaterial::ApplyValue(Instance, Type, Info, Value))
			{
				Written.Add(Instance);
			}
		}

		MoonToonMaterial::FinishEdits(Written);

		FString Report = FString::Printf(TEXT("%s = %s on %d of %d instance(s).\n"),
			*ParameterName.ToString(), ValueText, Written.Num(), Targets.Num());
		if (Skipped.Num() > 0)
		{
			Report += FString::Printf(TEXT("Not a parameter of: %s\n"), *FString::Join(Skipped, TEXT(", ")));
		}
		return Report;
	}
}

FString UMoonToonMaterialLibrary::SetScalarOnInstances(
	const TArray<UMaterialInstanceConstant*>& Instances,
	FName ParameterName,
	float Value)
{
	return SetOnInstances(Instances, EMaterialParameterType::Scalar, ParameterName,
		FMaterialParameterValue(Value), *FString::SanitizeFloat(Value));
}

FString UMoonToonMaterialLibrary::SetStaticSwitchOnInstances(
	const TArray<UMaterialInstanceConstant*>& Instances,
	FName ParameterName,
	bool bValue)
{
	return SetOnInstances(Instances, EMaterialParameterType::StaticSwitch, ParameterName,
		FMaterialParameterValue(bValue), bValue ? TEXT("true") : TEXT("false"));
}

FString UMoonToonMaterialLibrary::ResetOverrides(
	const TArray<UMaterialInstanceConstant*>& Instances,
	const TArray<FName>& ParameterNames)
{
	const TArray<UMaterialInstanceConstant*> Targets = Sanitize(Instances);
	if (Targets.Num() == 0)
	{
		return TEXT("No material instances given.");
	}

	const TSet<FName> Wanted(ParameterNames);
	FString Report;
	int32 TotalCleared = 0;

	for (UMaterialInstanceConstant* Instance : Targets)
	{
		TArray<TPair<EMaterialParameterType, FMaterialParameterInfo>> Overrides;
		CollectOverrides(Instance, Overrides);

		int32 NumCleared = 0;
		for (const TPair<EMaterialParameterType, FMaterialParameterInfo>& Override : Overrides)
		{
			if (Wanted.Num() > 0 && !Wanted.Contains(Override.Value.Name))
			{
				continue;
			}
			if (MoonToonMaterial::ClearOverride(Instance, Override.Key, Override.Value))
			{
				++NumCleared;
			}
		}

		TotalCleared += NumCleared;
		Report += FString::Printf(TEXT("  %-40s %d reset to parent\n"), *Instance->GetName(), NumCleared);
	}

	MoonToonMaterial::FinishEdits(Targets);
	return FString::Printf(TEXT("Reset %d override(s) across %d instance(s).\n\n%s"),
		TotalCleared, Targets.Num(), *Report);
}

FString UMoonToonMaterialLibrary::CaptureToPreset(
	UMoonToonMaterialPreset* Preset,
	const TArray<UMaterialInstanceConstant*>& Instances,
	bool bOverriddenOnly)
{
	const TArray<UMaterialInstanceConstant*> Sources = Sanitize(Instances);
	if (!Preset)
	{
		return TEXT("No preset given.");
	}
	if (Sources.Num() == 0)
	{
		return TEXT("No material instances given.");
	}

	TArray<FMoonToonMaterialParam> Params;
	int32 Skipped = 0;
	MoonToonMaterial::GatherSharedParameters(Sources, Params, Skipped);

	const FScopedTransaction Transaction(LOCTEXT("CapturePresetTransaction", "Capture Material Preset"));
	Preset->Modify();

	Preset->Scalars.Reset();
	Preset->Vectors.Reset();
	Preset->Textures.Reset();
	Preset->Switches.Reset();
	// Through the raw pointer: TObjectPtr to TSoftObjectPtr has no direct conversion.
	Preset->CapturedFromParent = ToRawPtr(Sources[0]->Parent);

	int32 NumDisagreed = 0;
	for (const FMoonToonMaterialParam& Param : Params)
	{
		if (bOverriddenOnly && Param.NumOverriding == 0)
		{
			continue;
		}
		if (!Param.bSameValue)
		{
			// Capturing one of several values would silently pick a winner.
			++NumDisagreed;
			continue;
		}

		switch (Param.Type)
		{
		case EMaterialParameterType::Scalar:
			Preset->Scalars.Add({ Param.Info.Name, Param.Value.AsScalar() });
			break;
		case EMaterialParameterType::Vector:
			Preset->Vectors.Add({ Param.Info.Name, Param.Value.AsLinearColor() });
			break;
		case EMaterialParameterType::Texture:
			Preset->Textures.Add({ Param.Info.Name, Param.Value.Texture });
			break;
		case EMaterialParameterType::StaticSwitch:
			Preset->Switches.Add({ Param.Info.Name, Param.Value.AsStaticSwitch() });
			break;
		default:
			break;
		}
	}

	Preset->MarkPackageDirty();

	FString Report = FString::Printf(
		TEXT("Captured %d value(s) into %s\n\n  %d scalar, %d vector, %d texture, %d switch\n"),
		Preset->NumEntries(), *Preset->GetName(),
		Preset->Scalars.Num(), Preset->Vectors.Num(), Preset->Textures.Num(), Preset->Switches.Num());
	if (NumDisagreed > 0)
	{
		Report += FString::Printf(
			TEXT("  %d parameter(s) left out: the selected instances do not agree on them.\n"), NumDisagreed);
	}
	if (Skipped > 0)
	{
		Report += FString::Printf(
			TEXT("  %d parameter(s) left out: not present on every selected instance.\n"), Skipped);
	}
	Report += TEXT("\nThe preset is not saved yet.\n");
	return Report;
}

FString UMoonToonMaterialLibrary::ApplyPreset(
	UMoonToonMaterialPreset* Preset,
	const TArray<UMaterialInstanceConstant*>& Instances)
{
	const TArray<UMaterialInstanceConstant*> Targets = Sanitize(Instances);
	if (!Preset)
	{
		return TEXT("No preset given.");
	}
	if (Targets.Num() == 0)
	{
		return TEXT("No material instances given.");
	}

	const FScopedTransaction Transaction(LOCTEXT("ApplyPresetTransaction", "Apply Material Preset"));

	FString Report;
	int32 TotalWritten = 0;

	for (UMaterialInstanceConstant* Instance : Targets)
	{
		Instance->Modify();

		int32 Written = 0;
		TArray<FString> Missing;

		auto Write = [&](EMaterialParameterType Type, FName Name, const FMaterialParameterValue& Value)
		{
			const FMaterialParameterInfo Info(Name);
			if (!MoonToonMaterial::ExistsInParent(Instance, Type, Info)
				&& !MoonToonMaterial::IsOverriddenHere(Instance, Type, Info))
			{
				Missing.Add(FString::Printf(TEXT("%s (%s)"), *Name.ToString(), TypeName(Type)));
				return;
			}
			if (MoonToonMaterial::ApplyValue(Instance, Type, Info, Value))
			{
				++Written;
			}
		};

		for (const FMoonToonPresetScalar& Entry : Preset->Scalars)
		{
			Write(EMaterialParameterType::Scalar, Entry.Name, FMaterialParameterValue(Entry.Value));
		}
		for (const FMoonToonPresetVector& Entry : Preset->Vectors)
		{
			Write(EMaterialParameterType::Vector, Entry.Name, FMaterialParameterValue(Entry.Value));
		}
		for (const FMoonToonPresetTexture& Entry : Preset->Textures)
		{
			// Soft on the asset, hard for the write: the instance needs the object, not the path.
			Write(EMaterialParameterType::Texture, Entry.Name, FMaterialParameterValue(Entry.Value.LoadSynchronous()));
		}
		for (const FMoonToonPresetSwitch& Entry : Preset->Switches)
		{
			Write(EMaterialParameterType::StaticSwitch, Entry.Name, FMaterialParameterValue(Entry.bValue));
		}

		TotalWritten += Written;
		Report += FString::Printf(TEXT("  %-40s %d written"), *Instance->GetName(), Written);
		if (Missing.Num() > 0)
		{
			Missing.Sort();
			Report += FString::Printf(TEXT(", %d not on this material: %s"),
				Missing.Num(), *FString::Join(Missing, TEXT(", ")));
		}
		Report += TEXT("\n");
	}

	MoonToonMaterial::FinishEdits(Targets);

	return FString::Printf(TEXT("Applied %s (%d value(s)) to %d instance(s), %d write(s).\n\n%s\nNothing is saved yet.\n"),
		*Preset->GetName(), Preset->NumEntries(), Targets.Num(), TotalWritten, *Report);
}

int32 UMoonToonMaterialLibrary::SaveInstances(const TArray<UMaterialInstanceConstant*>& Instances)
{
	int32 NumSaved = 0;
	for (UMaterialInstanceConstant* Instance : Sanitize(Instances))
	{
		UPackage* Package = Instance->GetOutermost();
		if (!Package || !Package->IsDirty())
		{
			continue;
		}

		const FString FileName = FPackageName::LongPackageNameToFilename(
			Package->GetName(), FPackageName::GetAssetPackageExtension());

		FSavePackageArgs SaveArgs;
		SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
		SaveArgs.SaveFlags = SAVE_NoError;
		if (UPackage::SavePackage(Package, nullptr, *FileName, SaveArgs))
		{
			++NumSaved;
		}
	}
	return NumSaved;
}

FString UMoonToonMaterialLibrary::DescribeInstances(const TArray<UMaterialInstanceConstant*>& Instances)
{
	const TArray<UMaterialInstanceConstant*> Targets = Sanitize(Instances);
	if (Targets.Num() == 0)
	{
		return TEXT("No material instances given.");
	}

	FString Report;
	for (UMaterialInstanceConstant* Instance : Targets)
	{
		TArray<TPair<EMaterialParameterType, FMaterialParameterInfo>> Overrides;
		CollectOverrides(Instance, Overrides);

		TArray<FString> Live;
		TArray<FString> Orphaned;
		for (const TPair<EMaterialParameterType, FMaterialParameterInfo>& Override : Overrides)
		{
			const FString Name = FString::Printf(TEXT("%s (%s)"), *Override.Value.Name.ToString(), TypeName(Override.Key));
			if (MoonToonMaterial::ExistsInParent(Instance, Override.Key, Override.Value))
			{
				Live.Add(Name);
			}
			else
			{
				Orphaned.Add(Name);
			}
		}

		Report += FString::Printf(TEXT("%s\n  parent    : %s\n  overrides : %d\n"),
			*Instance->GetName(),
			Instance->Parent ? *Instance->Parent->GetPathName() : TEXT("<none>"),
			Live.Num() + Orphaned.Num());
		if (Live.Num() > 0)
		{
			Report += FString::Printf(TEXT("  live      : %s\n"), *FString::Join(Live, TEXT(", ")));
		}
		if (Orphaned.Num() > 0)
		{
			Report += FString::Printf(TEXT("  orphaned  : %s\n"), *FString::Join(Orphaned, TEXT(", ")));
		}
		Report += TEXT("\n");
	}
	return Report;
}

#undef LOCTEXT_NAMESPACE
