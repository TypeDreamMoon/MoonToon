// Copyright Dream Moon. All Rights Reserved.

#include "MoonToonMaterialPresetFactory.h"

#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "Modules/ModuleManager.h"
#include "MoonToonMaterialPreset.h"

#define LOCTEXT_NAMESPACE "MoonToonMaterialPresetFactory"

UMoonToonMaterialPresetFactory::UMoonToonMaterialPresetFactory()
{
	SupportedClass = UMoonToonMaterialPreset::StaticClass();
	bCreateNew = true;
	bEditAfterNew = true;
}

UObject* UMoonToonMaterialPresetFactory::FactoryCreateNew(
	UClass* InClass,
	UObject* InParent,
	FName InName,
	EObjectFlags Flags,
	UObject* Context,
	FFeedbackContext* Warn)
{
	return NewObject<UMoonToonMaterialPreset>(InParent, InClass, InName, Flags);
}

FText UMoonToonMaterialPresetFactory::GetDisplayName() const
{
	return LOCTEXT("DisplayName", "MoonToon Material Preset");
}

uint32 UMoonToonMaterialPresetFactory::GetMenuCategories() const
{
	return FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get().FindAdvancedAssetCategory(
		FName(TEXT("Material")));
}

#undef LOCTEXT_NAMESPACE
