// Copyright Dream Moon. All Rights Reserved.

#include "MoonToonSubMaterialTable.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/Texture2D.h"
#include "Math/Float16Color.h"
#include "MaterialEditingLibrary.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"

DEFINE_LOG_CATEGORY_STATIC(LogMoonToonSubMaterial, Log, All);

namespace MoonToonSubMaterial
{
	// Override bits -- Tools/SubMaterial/submat_common.py FIELDS. BaseColorTint and its strength share bit 7.
	enum EBit : uint32
	{
		ShadowColor = 0, Metallic = 1, Specular = 2, Roughness = 3, Reflection = 4, Anisotropy = 5,
		RampUVOffset = 6, BaseColorTint = 7, EmissiveScale = 8,
		ScalarA = 9, ScalarB = 10, ScalarC = 11, ScalarD = 12, ScalarE = 13, ScalarF = 14, ScalarG = 15,
		VectorX = 16, VectorY = 17, VectorZ = 18, VectorW = 19,
		OutlineWidthScale = 20, OutlineColorScale = 21,
	};

	static const FName UseTableSwitch(TEXT("Use SubMaterial Table"));
	static const FName TableTextureParameter(TEXT("SubMaterial Table"));
}

int32 UMoonToonSubMaterialTable::GetOverrideMask(const FMoonToonSubMaterialRow& Row)
{
	using namespace MoonToonSubMaterial;
	uint32 Mask = 0;
	auto Set = [&Mask](bool bOn, EBit Bit) { Mask |= bOn ? (1u << Bit) : 0u; };
	Set(Row.bOverride_ShadowColor, ShadowColor);
	Set(Row.bOverride_Metallic, Metallic);
	Set(Row.bOverride_Specular, Specular);
	Set(Row.bOverride_Roughness, Roughness);
	Set(Row.bOverride_Reflection, Reflection);
	Set(Row.bOverride_Anisotropy, Anisotropy);
	Set(Row.bOverride_RampUVOffset, RampUVOffset);
	Set(Row.bOverride_BaseColorTint, BaseColorTint);
	Set(Row.bOverride_EmissiveScale, EmissiveScale);
	Set(Row.bOverride_ScalarA, ScalarA);
	Set(Row.bOverride_ScalarB, ScalarB);
	Set(Row.bOverride_ScalarC, ScalarC);
	Set(Row.bOverride_ScalarD, ScalarD);
	Set(Row.bOverride_ScalarE, ScalarE);
	Set(Row.bOverride_ScalarF, ScalarF);
	Set(Row.bOverride_ScalarG, ScalarG);
	Set(Row.bOverride_VectorX, VectorX);
	Set(Row.bOverride_VectorY, VectorY);
	Set(Row.bOverride_VectorZ, VectorZ);
	Set(Row.bOverride_VectorW, VectorW);
	Set(Row.bOverride_OutlineWidthScale, OutlineWidthScale);
	Set(Row.bOverride_OutlineColorScale, OutlineColorScale);
	return static_cast<int32>(Mask);
}

TArray<FLinearColor> UMoonToonSubMaterialTable::PackTexels() const
{
	TArray<FLinearColor> Texels;
	Texels.SetNumZeroed(MaxRows * NumColumns);

	const int32 NumRows = FMath::Min(Rows.Num(), MaxRows);
	for (int32 RowIndex = 0; RowIndex < NumRows; ++RowIndex)
	{
		const FMoonToonSubMaterialRow& Row = Rows[RowIndex];
		FLinearColor* C = &Texels[RowIndex * NumColumns];

		// A field that is not overridden stays 0, exactly as build_texels writes it.
		auto Put = [](bool bOn, float& Dst, float Value) { if (bOn) { Dst = Value; } };

		Put(Row.bOverride_ShadowColor, C[0].R, Row.ShadowColor.R);
		Put(Row.bOverride_ShadowColor, C[0].G, Row.ShadowColor.G);
		Put(Row.bOverride_ShadowColor, C[0].B, Row.ShadowColor.B);

		Put(Row.bOverride_Metallic, C[1].R, Row.Metallic);
		Put(Row.bOverride_Specular, C[1].G, Row.Specular);
		Put(Row.bOverride_Roughness, C[1].B, Row.Roughness);
		Put(Row.bOverride_Reflection, C[1].A, Row.Reflection);

		Put(Row.bOverride_Anisotropy, C[2].R, Row.Anisotropy);
		Put(Row.bOverride_RampUVOffset, C[2].G, Row.RampUVOffset);
		Put(Row.bOverride_BaseColorTint, C[2].B, Row.BaseColorTintStrength);
		Put(Row.bOverride_EmissiveScale, C[2].A, Row.EmissiveScale);

		Put(Row.bOverride_BaseColorTint, C[3].R, Row.BaseColorTint.R);
		Put(Row.bOverride_BaseColorTint, C[3].G, Row.BaseColorTint.G);
		Put(Row.bOverride_BaseColorTint, C[3].B, Row.BaseColorTint.B);

		Put(Row.bOverride_ScalarA, C[4].R, Row.ScalarA);
		Put(Row.bOverride_ScalarB, C[4].G, Row.ScalarB);
		Put(Row.bOverride_ScalarC, C[4].B, Row.ScalarC);
		Put(Row.bOverride_ScalarD, C[4].A, Row.ScalarD);

		Put(Row.bOverride_ScalarE, C[5].R, Row.ScalarE);
		Put(Row.bOverride_ScalarF, C[5].G, Row.ScalarF);
		Put(Row.bOverride_ScalarG, C[5].B, Row.ScalarG);

		Put(Row.bOverride_VectorX, C[6].R, Row.VectorX);
		Put(Row.bOverride_VectorY, C[6].G, Row.VectorY);
		Put(Row.bOverride_VectorZ, C[6].B, Row.VectorZ);
		Put(Row.bOverride_VectorW, C[6].A, Row.VectorW);

		Put(Row.bOverride_OutlineWidthScale, C[7].R, Row.OutlineWidthScale);
		Put(Row.bOverride_OutlineColorScale, C[7].G, Row.OutlineColorScale.R);
		Put(Row.bOverride_OutlineColorScale, C[7].B, Row.OutlineColorScale.G);
		Put(Row.bOverride_OutlineColorScale, C[7].A, Row.OutlineColorScale.B);

		// The mask in three bytes (column 0 .w, column 3 .w, column 5 .w): 0..255 is exact in half float.
		const uint32 Mask = static_cast<uint32>(GetOverrideMask(Row));
		C[0].A = static_cast<float>(Mask & 0xFFu);
		C[3].A = static_cast<float>((Mask >> 8) & 0xFFu);
		C[5].A = static_cast<float>((Mask >> 16) & 0xFFu);
	}
	return Texels;
}

UTexture2D* UMoonToonSubMaterialTable::BakeTexture()
{
#if WITH_EDITOR
	if (Rows.Num() > MaxRows)
	{
		UE_LOG(LogMoonToonSubMaterial, Warning, TEXT("%s: %d rows, only the first %d are baked."), *GetPathName(), Rows.Num(), MaxRows);
	}

	const TArray<FLinearColor> Texels = PackTexels();
	TArray<FFloat16Color> HalfTexels;
	HalfTexels.Reserve(Texels.Num());
	for (const FLinearColor& Texel : Texels)
	{
		HalfTexels.Add(FFloat16Color(Texel));
	}

	UTexture2D* Texture = BakedTexture;
	if (!Texture)
	{
		const FString PackageName = FPackageName::GetLongPackagePath(GetOutermost()->GetName()) / (TEXT("T_") + GetName());
		const FString AssetName = FPackageName::GetShortName(PackageName);
		Texture = LoadObject<UTexture2D>(nullptr, *(PackageName + TEXT(".") + AssetName), nullptr, LOAD_NoWarn | LOAD_Quiet);
		if (!Texture)
		{
			UPackage* Package = CreatePackage(*PackageName);
			if (UObject* Existing = StaticFindObject(nullptr, Package, *AssetName))
			{
				UE_LOG(LogMoonToonSubMaterial, Error, TEXT("%s: cannot bake, %s is already a %s."),
					*GetPathName(), *Existing->GetPathName(), *Existing->GetClass()->GetName());
				return nullptr;
			}
			Texture = NewObject<UTexture2D>(Package, *AssetName, RF_Public | RF_Standalone | RF_Transactional);
			FAssetRegistryModule::AssetCreated(Texture);
		}
	}

	Texture->Modify();
	Texture->PreEditChange(nullptr);
	Texture->Source.Init(NumColumns, MaxRows, 1, 1, TSF_RGBA16F, reinterpret_cast<const uint8*>(HalfTexels.GetData()));
	Texture->CompressionSettings = TC_HDR;
	Texture->SRGB = false;
	Texture->MipGenSettings = TMGS_NoMipmaps;
	Texture->Filter = TF_Nearest;
	Texture->NeverStream = true;
	Texture->LODGroup = TEXTUREGROUP_Pixels2D;
	Texture->AddressX = TA_Clamp;
	Texture->AddressY = TA_Clamp;
	Texture->PostEditChange();
	Texture->MarkPackageDirty();

	if (BakedTexture != Texture)
	{
		Modify();
		BakedTexture = Texture;
	}
	return Texture;
#else
	return BakedTexture;
#endif
}

bool UMoonToonSubMaterialTable::ApplyToMaterialInstance(UMaterialInstanceConstant* Instance)
{
#if WITH_EDITOR
	if (!Instance)
	{
		return false;
	}
	UTexture2D* Texture = BakedTexture ? BakedTexture.Get() : BakeTexture();
	if (!Texture)
	{
		return false;
	}
	// The stock setters never set their result (always false), so both values are read back instead.
	UMaterialEditingLibrary::SetMaterialInstanceStaticSwitchParameterValue(Instance, MoonToonSubMaterial::UseTableSwitch, true);
	UMaterialEditingLibrary::SetMaterialInstanceTextureParameterValue(Instance, MoonToonSubMaterial::TableTextureParameter, Texture);
	UMaterialEditingLibrary::UpdateMaterialInstance(Instance);
	const bool bSwitch = UMaterialEditingLibrary::GetMaterialInstanceStaticSwitchParameterValue(Instance, MoonToonSubMaterial::UseTableSwitch);
	const bool bTexture = UMaterialEditingLibrary::GetMaterialInstanceTextureParameterValue(Instance, MoonToonSubMaterial::TableTextureParameter) == Texture;
	if (!bSwitch || !bTexture)
	{
		UE_LOG(LogMoonToonSubMaterial, Warning, TEXT("%s: the material has no '%s' switch / '%s' texture parameter (not an M_MoonToon instance?)."),
			*Instance->GetPathName(), *MoonToonSubMaterial::UseTableSwitch.ToString(), *MoonToonSubMaterial::TableTextureParameter.ToString());
	}
	return bSwitch && bTexture;
#else
	return false;
#endif
}

#if WITH_EDITOR
void UMoonToonSubMaterialTable::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);

	// Only edits to the rows re-bake; ticking bAutoBake itself or a transient interactive drag does not.
	const FName MemberName = PropertyChangedEvent.GetMemberPropertyName();
	if (bAutoBake
		&& MemberName == GET_MEMBER_NAME_CHECKED(UMoonToonSubMaterialTable, Rows)
		&& PropertyChangedEvent.ChangeType != EPropertyChangeType::Interactive)
	{
		BakeTexture();
	}
}
#endif
