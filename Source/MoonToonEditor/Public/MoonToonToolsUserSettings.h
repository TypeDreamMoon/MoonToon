// Copyright Dream Moon. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "MoonToonToolsUserSettings.generated.h"

/**
 * Per-user, per-project state for the tools panel.
 *
 * Only things whose value is entirely in surviving the editor closing. A pinned parameter list that
 * resets every session is not a feature, and there is nothing here worth putting in source control,
 * which is why this is EditorPerProjectUserSettings rather than a project setting.
 */
UCLASS(config = EditorPerProjectUserSettings)
class MOONTOONEDITOR_API UMoonToonToolsUserSettings : public UObject
{
	GENERATED_BODY()

public:
	static UMoonToonToolsUserSettings& Get();

	/** Parameters pinned to the top of the material list, by name. */
	UPROPERTY(config)
	TArray<FName> FavoriteParameters;

	bool IsFavorite(FName ParameterName) const { return FavoriteParameters.Contains(ParameterName); }

	/** Adds or removes, and writes the ini straight away -- there is no other save point. */
	void ToggleFavorite(FName ParameterName);
};
