// Copyright Dream Moon. All Rights Reserved.

#include "MoonToonSubMaterialTableFactory.h"

#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "Modules/ModuleManager.h"
#include "MoonToonSubMaterialTable.h"

#define LOCTEXT_NAMESPACE "MoonToonSubMaterialTableFactory"

UMoonToonSubMaterialTableFactory::UMoonToonSubMaterialTableFactory()
{
	SupportedClass = UMoonToonSubMaterialTable::StaticClass();
	bCreateNew = true;
	bEditAfterNew = true;
}

UObject* UMoonToonSubMaterialTableFactory::FactoryCreateNew(
	UClass* InClass,
	UObject* InParent,
	FName InName,
	EObjectFlags Flags,
	UObject* Context,
	FFeedbackContext* Warn)
{
	return NewObject<UMoonToonSubMaterialTable>(InParent, InClass, InName, Flags);
}

FText UMoonToonSubMaterialTableFactory::GetDisplayName() const
{
	return LOCTEXT("DisplayName", "MoonToon Sub-Material Table");
}

uint32 UMoonToonSubMaterialTableFactory::GetMenuCategories() const
{
	return FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get().FindAdvancedAssetCategory(
		FName(TEXT("Material")));
}

#undef LOCTEXT_NAMESPACE
