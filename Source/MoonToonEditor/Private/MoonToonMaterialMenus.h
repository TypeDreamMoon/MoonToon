// Copyright Dream Moon. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "ContentBrowserDelegates.h"

/**
 * Registers the MoonToon section of the Material Instance context menu in the Content Browser.
 *
 * Call inside a FToolMenuOwnerScoped, from the module's ToolMenus startup callback -- the same place
 * the mesh entries are registered.
 */
void RegisterMoonToonMaterialMenus();

/**
 * A Content Browser asset picker sized to sit inside a menu.
 *
 * Shared by the Content Browser entries and the tools panel's section menu so that "pick a material"
 * is the same widget, with the same search and the same size, wherever it is asked for.
 */
TSharedRef<class SWidget> MakeMoonToonAssetPickerMenu(const UClass* AllowedClass, FOnAssetSelected OnSelected);
