// Copyright Dream Moon. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Factories/Factory.h"
#include "MoonToonSubMaterialTableFactory.generated.h"

/** Right-click > Material > MoonToon Sub-Material Table in the Content Browser. */
UCLASS()
class MOONTOONEDITOR_API UMoonToonSubMaterialTableFactory : public UFactory
{
	GENERATED_BODY()

public:
	UMoonToonSubMaterialTableFactory();

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
