// Copyright Dream Moon. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Factories/Factory.h"
#include "MoonToonMaterialPresetFactory.generated.h"

/**
 * Makes the preset a first-class asset: right-click in the Content Browser, and the save dialog the
 * panel's "save as new preset" uses.
 */
UCLASS()
class MOONTOONEDITOR_API UMoonToonMaterialPresetFactory : public UFactory
{
	GENERATED_BODY()

public:
	UMoonToonMaterialPresetFactory();

	virtual UObject* FactoryCreateNew(
		UClass* InClass,
		UObject* InParent,
		FName InName,
		EObjectFlags Flags,
		UObject* Context,
		FFeedbackContext* Warn) override;

	virtual FText GetDisplayName() const override;
	virtual uint32 GetMenuCategories() const override;
};
