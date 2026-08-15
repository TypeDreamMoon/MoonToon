// Copyright Dream Moon. All Rights Reserved.

#include "MoonToonToolsUserSettings.h"

UMoonToonToolsUserSettings& UMoonToonToolsUserSettings::Get()
{
	return *GetMutableDefault<UMoonToonToolsUserSettings>();
}

void UMoonToonToolsUserSettings::ToggleFavorite(FName ParameterName)
{
	if (FavoriteParameters.Remove(ParameterName) == 0)
	{
		FavoriteParameters.Add(ParameterName);
	}
	SaveConfig();
}
